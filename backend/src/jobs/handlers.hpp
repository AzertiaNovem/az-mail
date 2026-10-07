// Owner: WP-C
//
// Job handlers (DESIGN "Outbound pipeline", "Inbound pipeline", A6, B5–B10) and their
// registration. Implemented in outbound_jobs.cpp, inbound_jobs.cpp, maintenance_jobs.cpp.
// Handlers do network I/O OUTSIDE transactions and only short Pool::write calls around it.
// Rate-limiter priorities: outbound.send High, inbound.fetch Normal, the rest Low.
// Common rules:
//  * Last attempt: when job.attempts >= job.max_attempts, ANY failure that would consume the
//    attempt (everything except Retry{count_attempt=false}) first records the terminal state —
//    mail::mark_failed (outbound.send) / mail::mark_inbound_failed (inbound.fetch) — in its own
//    short write, then rethrows, so no row is left 'queued'/'sending'/'pending' behind a dead job.
//  * Long downloads/uploads (inbound attachments up to 50 MB) call jobs::extend_lease before each
//    transfer; a false return means the lease was lost (another worker owns the job) → stop
//    without side effects (throw Retry{}).
//  * Times come from svc.clock (svc.now_ms()), never azm::now_ms() directly.
#pragma once

#include "core/blob_store.hpp"
#include "jobs/jobs.hpp"
#include "mail/eml.hpp"
#include "mail/types.hpp"
#include "resend/types.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace azm {
struct Services;
}

namespace azm::jobs {

// ---- registration (called by app::App before Runner::start) --------------------------------
// outbound.send, outbound.fetch_meta, outbound.reconcile (periodic cfg.reconcile_interval_sec).
void register_outbound_jobs(Runner& runner);
// inbound.fetch, poll.receiving (periodic cfg.poll_interval_sec).
void register_inbound_jobs(Runner& runner);
// purge.trash, gc.blobs, gc.housekeeping, db.optimize (periodic, kinds.hpp intervals).
void register_maintenance_jobs(Runner& runner);
// All three.
void register_all_jobs(Runner& runner);

// ---- outbound_jobs.cpp ---------------------------------------------------------------------
// outbound.send {outbound_id}: mark_sending (false → return); resolve a missing parent
// Message-ID (GET parent resend id → set_outbound_message_id → reload plan); attachments via
// Services::blobs_for(storage).get_bytes (≤ cfg.upload_body_limit) → base64; client.send; then
// one transaction: mark_accepted + enqueue outbound.fetch_meta (+10 s, or scheduled_at + 60 s).
// Errors: RateLimited → Retry{retry_after, count_attempt=false}; IdempotencyInFlight → Retry{2 s};
// Network/Server → note_send_retry + Retry{outbound_backoff(attempts)}; Quota → mark_failed
// ("发送配额已用完") + kv quota_blocked + Permanent; Validation/Auth/IdempotencyConflict →
// mark_failed + Permanent; Validation mentioning scheduling with scheduled_via=resend →
// switch_to_local_schedule + Retry{until scheduled_at, count_attempt=false}. On the last
// attempt any error (retryable or not, incl. BlobError / unexpected exceptions) ends in
// mark_failed before rethrowing (common rules above).
void run_outbound_send(Services& svc, const Job& job, std::stop_token st);
// outbound.fetch_meta {outbound_id}: client.get(resend_id, Low) → set_outbound_message_id when
// present; last_event applied as an OutboundEvent (source_key "poll:<last_event>"); message_id
// still null → Retry (backoff).
void run_outbound_fetch_meta(Services& svc, const Job& job, std::stop_token st);
// outbound.reconcile (periodic): for mail::outbound_to_reconcile(now, 7 d, 50) → client.get →
// apply_outbound_event("email.<last_event>", source_key "poll:<last_event>").
void run_outbound_reconcile(Services& svc, const Job& job, std::stop_token st);

// Retry delay for outbound.send after `attempts` attempts: 5 s, 15 s, 1 min, 5 min, 15 min,
// 1 h, 2 h, 4 h, 8 h (then 8 h).
std::chrono::milliseconds outbound_backoff(int attempts);

// ---- inbound_jobs.cpp ----------------------------------------------------------------------
// inbound.fetch {resend_id, source}: get_received (NotFound → Retry 30 s/1/2/3/4 min, then
// mark_inbound_failed + Permanent); raw → tmp (download_to, hash); header block via
// eml::read_file_prefix + parse_headers; list_received_attachments (fresh URLs) → each
// download_to (≤ cfg.inbound_attachment_limit) into tmp; then, under ONE blob_writer_guard()
// (core/blob_store.hpp) held until the delivery transaction has committed: put_file for the raw
// and every attachment, then one transaction mail::deliver_inbound(build_inbound_email(...)).
// Downloads happen before the guard so gc.blobs is never blocked by slow transfers. Staged temp
// files left by a failure are removed before rethrowing. Last attempt → mark_inbound_failed
// (common rules above), whatever the error.
void run_inbound_fetch(Services& svc, const Job& job, std::stop_token st);
// poll.receiving (periodic / manual): list_received newest-first, walk pages until a known id
// (mail::inbound_state) or 20 pages; record_inbound_pending + enqueue inbound.fetch (source
// poll) per unknown id; kv last_poll_at / poll.high_water; warns (kv poll.gap_warning) when
// ids older than the high-water mark were never seen (B7).
void run_poll_receiving(Services& svc, const Job& job, std::stop_token st);

// Pure mapping of a fetched email + parsed raw headers + stored blobs to the domain input.
// Header values win over Resend fields for Message-ID / In-Reply-To / References /
// X-AzMail-Ref / Auto-Submitted / Date; Resend's headers map is the fallback when there is no
// raw; bcc is dropped (C2); addresses parsed with parse_address_list.
mail::InboundEmail build_inbound_email(const resend::ReceivedEmail& rcv,
                                       const mail::eml::ParsedHeaders& hdr,
                                       std::optional<BlobRef> raw,
                                       std::vector<mail::InboundAttachment> attachments,
                                       mail::InboundSource source);

// ---- maintenance_jobs.cpp ------------------------------------------------------------------
// purge.trash: mail::purge_trash(now, cfg.trash_purge_days, cfg.spam_purge_days, 500) until done.
void run_purge_trash(Services& svc, const Job& job, std::stop_token st);
// gc.blobs: R: mail::unreferenced_blobs(now - cfg.blob_gc_grace_hours, batch); then per blob,
// each under its own blob_gc_guard() (never across the batch, so writers wait for at most one
// DELETE): R: mail::is_blob_unreferenced (skip if referenced again) → svc.blobs_for(storage)
// .remove(sha) OUTSIDE any transaction (not in tx.after_commit; BlobError → log warn with the
// sha and error, keep the row so the next run retries, continue) → W:
// mail::forget_blob_if_unreferenced. R2BlobStore::remove already evicts its FileCache entry.
// Stops early when `st` is stopped.
void run_gc_blobs(Services& svc, const Job& job, std::stop_token st);
// gc.housekeeping: repo::purge_expired_sessions, jobs::purge_finished (cfg.jobs_done_retention_days),
// webhook_events older than cfg.webhook_events_retention_days, mail::purge_orphan_uploads
// (cfg.unattached_upload_ttl_hours), jobs::recover_expired_leases; then (outside any tx) deletes
// regular files in svc.blobs.tmp_dir() (and secondary_blobs->tmp_dir() when different) whose
// mtime is older than 24 h — staging .part files and temp downloads left by crashes.
void run_gc_housekeeping(Services& svc, const Job& job, std::stop_token st);
// db.optimize: PRAGMA optimize; PRAGMA wal_checkpoint(TRUNCATE).
void run_db_optimize(Services& svc, const Job& job, std::stop_token st);

}  // namespace azm::jobs
