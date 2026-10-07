// Owner: WP-C (frozen by WP0; new kinds are additive)
//
// Job kinds, lanes, priorities, payloads and dedupe keys (DESIGN §7 "Integration contracts",
// A6). jobs::enqueue derives the lane from the kind via lane_for(); Runner::on must register
// each kind on the same lane.
//
//  Kind                 Lane         Payload                         Dedupe key        Notes
//  outbound.send        outbound     {outbound_id}                   out:send:<id>     priority kPriorityHigh; run_at = send_after / scheduled_at (local)
//  outbound.fetch_meta  sync         {outbound_id}                   out:meta:<id>     +10 s after accept, or scheduled_at + 60 s
//  outbound.reconcile   maintenance  {}                              periodic          every cfg.reconcile_interval_sec (600 s)
//  inbound.fetch        inbound      {resend_id, source}             in:<resend_id>    source = "webhook"|"poll"|"admin"; priority kPriorityNormal
//  poll.receiving       sync         {} (or {manual:true})           periodic          every cfg.poll_interval_sec (120 s); admin sync uses dedupe "poll:manual"
//  purge.trash          maintenance  {}                              periodic          hourly
//  gc.blobs             maintenance  {}                              periodic          hourly
//  gc.housekeeping      maintenance  {}                              periodic          hourly (sessions, jobs, webhook_events, orphan uploads)
//  db.optimize          maintenance  {}                              periodic          daily (PRAGMA optimize + wal_checkpoint(TRUNCATE))
// Periodic jobs use dedupe key "periodic:<kind>" (see Runner::on).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azm::jobs {

namespace kinds {
inline constexpr std::string_view kOutboundSend = "outbound.send";
inline constexpr std::string_view kOutboundFetchMeta = "outbound.fetch_meta";
inline constexpr std::string_view kOutboundReconcile = "outbound.reconcile";
inline constexpr std::string_view kInboundFetch = "inbound.fetch";
inline constexpr std::string_view kPollReceiving = "poll.receiving";
inline constexpr std::string_view kPurgeTrash = "purge.trash";
inline constexpr std::string_view kGcBlobs = "gc.blobs";
inline constexpr std::string_view kGcHousekeeping = "gc.housekeeping";
inline constexpr std::string_view kDbOptimize = "db.optimize";
}  // namespace kinds

namespace lanes {
inline constexpr std::string_view kOutbound = "outbound";
inline constexpr std::string_view kInbound = "inbound";
inline constexpr std::string_view kSync = "sync";
inline constexpr std::string_view kMaintenance = "maintenance";
}  // namespace lanes

// jobs.priority (higher runs first within a lane).
inline constexpr int kPriorityHigh = 100;   // outbound.send
inline constexpr int kPriorityNormal = 50;  // inbound.fetch
inline constexpr int kPriorityLow = 0;      // everything else

// outbound.send: 1 + 9 retries on the backoff schedule of jobs::outbound_backoff (≈ 15.4 h in
// total, inside Resend's 24 h idempotency window, B1).
inline constexpr int kOutboundSendMaxAttempts = 10;

// Payload field names.
namespace payload {
inline constexpr std::string_view kOutboundId = "outbound_id";
inline constexpr std::string_view kResendId = "resend_id";
inline constexpr std::string_view kSource = "source";  // "webhook" | "poll" | "admin"
inline constexpr std::string_view kManual = "manual";  // poll.receiving triggered by admin sync
}  // namespace payload

// Lane of a known kind; nullopt for unknown kinds (enqueue then throws std::invalid_argument).
inline std::optional<std::string_view> lane_for(std::string_view kind) {
  if (kind == kinds::kOutboundSend) return lanes::kOutbound;
  if (kind == kinds::kInboundFetch) return lanes::kInbound;
  if (kind == kinds::kOutboundFetchMeta || kind == kinds::kPollReceiving) return lanes::kSync;
  if (kind == kinds::kOutboundReconcile || kind == kinds::kPurgeTrash || kind == kinds::kGcBlobs ||
      kind == kinds::kGcHousekeeping || kind == kinds::kDbOptimize)
    return lanes::kMaintenance;
  return std::nullopt;
}

// ---- dedupe keys -------------------------------------------------------------------------------
inline std::string dedupe_outbound_send(int64_t outbound_id) {
  return "out:send:" + std::to_string(outbound_id);
}
inline std::string dedupe_fetch_meta(int64_t outbound_id) {
  return "out:meta:" + std::to_string(outbound_id);
}
inline std::string dedupe_inbound_fetch(std::string_view resend_id) {
  return "in:" + std::string(resend_id);
}
inline std::string dedupe_periodic(std::string_view kind) { return "periodic:" + std::string(kind); }
inline constexpr std::string_view kDedupeManualPoll = "poll:manual";

// ---- periodic intervals not taken from Config ------------------------------------------------
inline constexpr std::chrono::seconds kPurgeTrashEvery{3600};
inline constexpr std::chrono::seconds kGcBlobsEvery{3600};
inline constexpr std::chrono::seconds kHousekeepingEvery{3600};
inline constexpr std::chrono::seconds kDbOptimizeEvery{86400};

}  // namespace azm::jobs
