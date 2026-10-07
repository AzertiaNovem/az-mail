// Owner: WP-C
// outbound.send / outbound.fetch_meta / outbound.reconcile (DESIGN "Outbound pipeline", B1–B5,
// B10). Network calls happen between short transactions, never inside one.
#include "core/crypto.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "db/kv.hpp"
#include "jobs/handlers.hpp"
#include "jobs/kinds.hpp"
#include "mail/outbound.hpp"
#include "resend/client.hpp"
#include "services.hpp"

#include <algorithm>

namespace azm::jobs {
namespace {

using namespace std::chrono_literals;
using resend::Error;

constexpr std::string_view kZhRetrying = "发送重试中";
constexpr std::string_view kZhFailed = "发送失败";
constexpr std::string_view kZhQuota = "发送配额已用完";
constexpr std::string_view kZhAuth = "邮件服务认证失败，请联系管理员";
constexpr std::string_view kZhConflict = "发送请求冲突，请重新发送";
constexpr std::string_view kZhAttachmentMissing = "附件文件丢失，无法发送";
constexpr std::string_view kZhAttachmentUnreadable = "附件无法读取，无法发送";
constexpr std::string_view kZhNotConfigured = "邮件服务未配置";

constexpr int64_t kReconcileMaxAgeMs = 7LL * 24 * 3600 * 1000;
constexpr int kReconcileBatch = 50;

int64_t outbound_id_of(const Job& job) {
  auto it = job.payload.find(payload::kOutboundId);
  if (it != job.payload.end()) {
    if (it->value().is_int64()) return it->value().as_int64();
    if (it->value().is_uint64()) return static_cast<int64_t>(it->value().as_uint64());
  }
  throw Permanent("payload has no outbound_id");
}

std::chrono::milliseconds retry_after(const Error& e) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(e.retry_after.value_or(std::chrono::seconds(1)));
}

std::string error_name(const Error& e) { return e.name.empty() ? std::string(resend::to_string(e.kind)) : e.name; }

bool mentions_scheduling(const Error& e) {
  return to_lower_ascii(e.message).find("schedul") != std::string::npos ||
         to_lower_ascii(e.name).find("schedul") != std::string::npos;
}

// Errors that must not consume an attempt: rate limiting and shutdown.
void rethrow_transient(const Error& e) {
  if (e.kind == Error::Kind::RateLimited) throw Retry(retry_after(e), "rate_limited", false);
  if (e.kind == Error::Kind::Network && e.name == "stopped") throw Retry(1s, "shutdown", false);
}

std::optional<std::string> normalized_msgid(const std::optional<std::string>& raw) {
  if (!raw) return std::nullopt;
  std::string id = mail::normalize_message_id(*raw);
  if (id.empty()) return std::nullopt;
  return id;
}

// One attempt of outbound.send. Records terminal failures itself (via `fail`) before throwing
// Permanent; transient problems are thrown as Retry with `last_name` set.
class SendAttempt {
 public:
  SendAttempt(Services& svc, const Job& job, int64_t id) : svc_(svc), job_(job), id_(id) {}

  std::string last_name = "internal_error";
  bool failure_recorded = false;

  void fail(std::string_view name, std::string_view detail_zh, std::optional<std::string_view> quota = {}) {
    if (failure_recorded) return;
    const int64_t now = svc_.now_ms();
    svc_.db.write([&](db::Tx& tx) {
      mail::mark_failed(tx, id_, name, detail_zh);
      if (quota) db::kv_set(tx, db::kv_keys::kQuotaBlocked, *quota, now);
    });
    failure_recorded = true;
  }

  void run() {
    if (svc_.resend == nullptr) {
      last_name = "not_configured";
      throw Retry(5min, "resend client not configured");
    }
    resend::Client& client = *svc_.resend;
    if (!svc_.db.write([&](db::Tx& tx) { return mail::mark_sending(tx, id_); })) return;  // canceled / done

    mail::OutboundSendPlan plan = load_plan();
    if (plan.parent_message_id_missing && plan.parent_resend_id && plan.parent_outbound_id) {
      // B2: capture the parent's Message-ID so In-Reply-To/References are complete.
      try {
        const auto parent = client.get(*plan.parent_resend_id, resend::Priority::High);
        if (auto mid = normalized_msgid(parent.message_id)) {
          svc_.db.write([&](db::Tx& tx) { mail::set_outbound_message_id(tx, *plan.parent_outbound_id, *mid); });
          plan = load_plan();
        }
      } catch (const Error& e) {
        rethrow_transient(e);
        log::warn("parent Message-ID unavailable; sending without it",
                  {{"outbound_id", id_}, {"error", error_name(e)}});
      }
    }

    resend::SendRequest req;
    req.from = plan.from;
    req.to = plan.to;
    req.cc = plan.cc;
    req.bcc = plan.bcc;
    req.reply_to = plan.reply_to;
    req.subject = plan.subject;
    req.html = plan.html;
    req.text = plan.text;
    req.headers = plan.headers;
    req.tags = plan.tags;
    // B1: the key is the outbound uuid. Locally scheduled rows use a distinct, equally stable
    // key: after Resend rejected a scheduled POST (B5 switch to local scheduling) the body
    // changes, and reusing the key could replay that rejection or answer 409.
    req.idempotency_key = plan.scheduled_via == mail::ScheduledVia::Local && plan.scheduled_at
                              ? plan.uuid + "-local"
                              : plan.uuid;
    for (const auto& a : plan.attachments) req.attachments.push_back(load_attachment(a));
    // Decided from the row only (never from "now"), so every retry sends the identical body
    // under the same Idempotency-Key (a changed body would be 409 invalid_idempotent_request).
    const bool remote_schedule = plan.scheduled_via == mail::ScheduledVia::Resend && plan.scheduled_at.has_value();
    if (remote_schedule) req.scheduled_at_iso = iso8601_utc(*plan.scheduled_at);

    std::string resend_id;
    try {
      resend_id = client.send(req);
    } catch (const Error& e) {
      handle_send_error(e, plan, remote_schedule);
    }

    const int64_t done_at = svc_.now_ms();
    svc_.db.write([&](db::Tx& tx) {
      mail::mark_accepted(tx, id_, resend_id, remote_schedule);
      const int64_t meta_at = remote_schedule ? *plan.scheduled_at + 60'000 : done_at + 10'000;
      enqueue(tx, kinds::kOutboundFetchMeta, {{std::string(payload::kOutboundId), id_}},
              {.run_at_ms = meta_at, .dedupe_key = dedupe_fetch_meta(id_), .now_ms = done_at});
      db::kv_delete(tx, db::kv_keys::kQuotaBlocked);  // a successful send clears the banner
    });
    log::info("outbound accepted", {{"outbound_id", id_}, {"resend_id", resend_id}, {"scheduled", remote_schedule}});
  }

 private:
  mail::OutboundSendPlan load_plan() {
    try {
      return svc_.db.read([&](db::Conn& c) { return mail::load_send_plan(c, id_); });
    } catch (const std::out_of_range&) {
      throw Permanent("outbound row missing");
    }
  }

  resend::OutAttachment load_attachment(const mail::PlanAttachment& a) {
    std::string bytes;
    try {
      bytes = svc_.blobs_for(a.storage).get_bytes(a.blob_sha256, svc_.cfg.upload_body_limit);
    } catch (const BlobNotFound&) {
      fail("attachment_missing", kZhAttachmentMissing);
      throw Permanent("attachment blob missing");
    } catch (const BlobError& e) {
      if (!e.retryable) {
        fail("attachment_unavailable", kZhAttachmentUnreadable);
        throw Permanent(std::string("attachment unreadable: ") + e.what());
      }
      last_name = "storage_unavailable";
      throw Retry(outbound_backoff(job_.attempts), std::string("storage: ") + e.what());
    }
    return {a.filename, a.content_type, crypto::b64_encode(bytes), a.content_id};
  }

  [[noreturn]] void handle_send_error(const Error& e, const mail::OutboundSendPlan& plan, bool remote_schedule) {
    rethrow_transient(e);
    const std::string name = error_name(e);
    switch (e.kind) {
      case Error::Kind::IdempotencyInFlight:
        last_name = name;
        throw Retry(2s, name);
      case Error::Kind::Network:
      case Error::Kind::Server:
        last_name = name;
        if (job_.attempts < job_.max_attempts) {
          try {
            svc_.db.write([&](db::Tx& tx) { mail::note_send_retry(tx, id_, name, kZhRetrying); });
          } catch (const std::exception& ex) {
            log::warn("cannot record send retry", {{"outbound_id", id_}, {"error", ex.what()}});
          }
        }
        throw Retry(outbound_backoff(job_.attempts), std::string(resend::to_string(e.kind)) + ": " + name);
      case Error::Kind::Quota:
        fail(name, kZhQuota, name == "monthly_quota_exceeded" ? "monthly" : "daily");
        throw Permanent("quota exceeded: " + name);
      case Error::Kind::Validation:
        if (remote_schedule && mentions_scheduling(e)) {
          // B5: Resend refused the schedule (e.g. attachments) → schedule locally instead.
          const int64_t at = svc_.db.write([&](db::Tx& tx) { return mail::switch_to_local_schedule(tx, id_); });
          log::info("Resend rejected scheduling; switched to local scheduling", {{"outbound_id", id_}});
          throw Retry(std::chrono::milliseconds(std::max<int64_t>(1, at - svc_.now_ms())), "local schedule", false);
        }
        fail(name, std::string(kZhFailed) + "：" + (e.message.empty() ? name : e.message));
        throw Permanent("rejected: " + name);
      case Error::Kind::Auth:
        fail(name, kZhAuth);
        throw Permanent("auth: " + name);
      case Error::Kind::IdempotencyConflict:
        fail(name, kZhConflict);
        throw Permanent("idempotency conflict");
      case Error::Kind::NotFound:
      case Error::Kind::RateLimited:  // handled above
        break;
    }
    fail(name, kZhFailed);
    throw Permanent("send failed: " + name);
  }

  Services& svc_;
  const Job& job_;
  int64_t id_;
};

}  // namespace

std::chrono::milliseconds outbound_backoff(int attempts) {
  static constexpr std::chrono::milliseconds kSteps[] = {5s, 15s, 1min, 5min, 15min, 1h, 2h, 4h, 8h};
  if (attempts <= 0) return kSteps[0];
  if (attempts > static_cast<int>(std::size(kSteps))) return 8h;
  return kSteps[attempts - 1];
}

void run_outbound_send(Services& svc, const Job& job, std::stop_token) {
  const int64_t id = outbound_id_of(job);
  const bool last = job.attempts >= job.max_attempts;
  SendAttempt attempt(svc, job, id);
  auto record_last = [&](std::string_view why) {
    // Common rule: no outbound row may stay queued/sending behind a dead job.
    try {
      attempt.fail(attempt.last_name, kZhFailed);
    } catch (const std::exception& e) {
      log::error("cannot mark outbound failed", {{"outbound_id", id}, {"reason", why}, {"error", e.what()}});
    }
  };
  try {
    attempt.run();
  } catch (const Retry& r) {
    if (r.count_attempt && last) record_last(r.reason);
    throw;
  } catch (const Permanent&) {
    throw;
  } catch (const std::exception& e) {
    if (svc.resend == nullptr) attempt.last_name = "not_configured";
    if (last) record_last(e.what());
    throw;
  }
}

void run_outbound_fetch_meta(Services& svc, const Job& job, std::stop_token) {
  const int64_t id = outbound_id_of(job);
  const auto row = svc.db.read([&](db::Conn& c) { return mail::get_outbound(c, id); });
  if (!row || row->status == mail::OutboundStatus::Canceled) return;
  if (!row->resend_id) {
    if (row->status == mail::OutboundStatus::Failed) return;
    throw Retry(std::chrono::milliseconds(0), "no resend id yet");
  }
  if (svc.resend == nullptr) throw Retry(5min, std::string(kZhNotConfigured));
  resend::SentEmail sent;
  try {
    sent = svc.resend->get(*row->resend_id, resend::Priority::Low);
  } catch (const Error& e) {
    rethrow_transient(e);
    if (e.kind == Error::Kind::NotFound) throw Permanent("email unknown to Resend");
    throw Retry(std::chrono::milliseconds(0), error_name(e));
  }
  const auto msgid = normalized_msgid(sent.message_id);
  const int64_t now = svc.now_ms();
  svc.db.write([&](db::Tx& tx) {
    if (msgid) mail::set_outbound_message_id(tx, id, *msgid);
    if (sent.last_event) {
      mail::OutboundEvent ev;
      ev.resend_id = row->resend_id;
      ev.uuid = row->uuid;
      ev.type = mail::event_type_for_last_event(*sent.last_event);
      ev.occurred_at = now;
      ev.source_key = "poll:" + *sent.last_event;
      ev.message_id = msgid;
      (void)mail::apply_outbound_event(tx, ev);
    }
  });
  if (!msgid && !row->message_id_header) throw Retry(std::chrono::milliseconds(0), "message_id not yet available");
}

void run_outbound_reconcile(Services& svc, const Job&, std::stop_token st) {
  if (svc.resend == nullptr) return;
  const int64_t now = svc.now_ms();
  const auto items = svc.db.read(
      [&](db::Conn& c) { return mail::outbound_to_reconcile(c, now, kReconcileMaxAgeMs, kReconcileBatch); });
  int applied = 0;
  for (const auto& item : items) {
    if (st.stop_requested()) break;
    resend::SentEmail sent;
    try {
      sent = svc.resend->get(item.resend_id, resend::Priority::Low);
    } catch (const Error& e) {
      if (e.kind == Error::Kind::NotFound) continue;
      if (e.kind == Error::Kind::RateLimited || e.kind == Error::Kind::Network) break;  // next period
      log::warn("reconcile GET failed", {{"outbound_id", item.outbound_id}, {"error", error_name(e)}});
      continue;
    }
    if (!sent.last_event) continue;
    const auto msgid = normalized_msgid(sent.message_id);
    svc.db.write([&](db::Tx& tx) {
      mail::OutboundEvent ev;
      ev.resend_id = item.resend_id;
      ev.type = mail::event_type_for_last_event(*sent.last_event);
      ev.occurred_at = svc.now_ms();
      ev.source_key = "poll:" + *sent.last_event;
      ev.message_id = msgid;
      if (mail::apply_outbound_event(tx, ev).status_changed) ++applied;
    });
  }
  if (applied > 0) log::info("reconcile applied status changes", {{"count", applied}});
}

void register_outbound_jobs(Runner& runner) {
  const Config& cfg = runner.services().cfg;
  runner.on(std::string(kinds::kOutboundSend), std::string(lanes::kOutbound), run_outbound_send);
  runner.on(std::string(kinds::kOutboundFetchMeta), std::string(lanes::kSync), run_outbound_fetch_meta);
  runner.on(std::string(kinds::kOutboundReconcile), std::string(lanes::kMaintenance), run_outbound_reconcile,
            std::chrono::seconds(std::max(60, cfg.reconcile_interval_sec)));
}

void register_all_jobs(Runner& runner) {
  register_outbound_jobs(runner);
  register_inbound_jobs(runner);
  register_maintenance_jobs(runner);
}

}  // namespace azm::jobs
