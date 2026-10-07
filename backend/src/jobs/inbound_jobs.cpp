// Owner: WP-C
// inbound.fetch and poll.receiving (DESIGN "Inbound pipeline", B7–B9, C8).
// All downloads happen before any transaction; blobs are stored under one blob_writer_guard()
// that is held until the delivery transaction has committed (CONTRACTS §G rule 2).
#include "core/address.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "db/kv.hpp"
#include "jobs/handlers.hpp"
#include "jobs/kinds.hpp"
#include "mail/eml.hpp"
#include "mail/inbound.hpp"
#include "resend/client.hpp"
#include "services.hpp"

#include <system_error>
#include <unordered_set>

namespace azm::jobs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using resend::Error;

// B7: GET receiving/{id} may 404 right after the webhook — retry 5 times over ~10 minutes.
constexpr std::chrono::milliseconds kNotFoundDelays[] = {30s, 1min, 2min, 3min, 4min};
constexpr int64_t kTransferLeaseMs = 10 * 60 * 1000;  // renewed before every download
constexpr int kPollPageSize = 100;
constexpr int kPollMaxPages = 20;  // B8

std::chrono::milliseconds retry_after(const Error& e) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(e.retry_after.value_or(std::chrono::seconds(1)));
}
std::string error_name(const Error& e) { return e.name.empty() ? std::string(resend::to_string(e.kind)) : e.name; }

// Addresses from Resend fields (each entry may itself hold a list); display names decoded.
std::vector<Address> addresses_of(const std::vector<std::string>& raw) {
  std::vector<Address> out;
  for (const auto& s : raw) {
    for (auto a : parse_address_list(mail::eml::unfold(s))) {
      a.name = std::string(trim(mail::eml::decode_rfc2047(a.name)));
      out.push_back(std::move(a));
    }
  }
  return out;
}

template <class T>
std::optional<T> first_of(const std::optional<T>& a, const std::optional<T>& b) {
  return a ? a : b;
}

// Removes staged temp files that were not consumed by put_file (which deletes on success).
struct StagedFiles {
  std::vector<fs::path> paths;
  ~StagedFiles() {
    for (const auto& p : paths) {
      std::error_code ec;
      fs::remove(p, ec);
    }
  }
};

}  // namespace

mail::InboundEmail build_inbound_email(const resend::ReceivedEmail& rcv, const mail::eml::ParsedHeaders& hdr,
                                       std::optional<BlobRef> raw, std::vector<mail::InboundAttachment> attachments,
                                       mail::InboundSource source) {
  // Resend's headers map: per-field fallback when the raw .eml is missing or lacks a header.
  mail::eml::HeaderBlock map_block;
  for (const auto& [k, v] : rcv.headers) map_block.headers.push_back({k, v});
  const mail::eml::ParsedHeaders fb = mail::eml::extract_headers(map_block);

  mail::InboundEmail e;
  e.resend_id = rcv.id;
  if (!hdr.from.empty()) {
    e.from = hdr.from.front();
  } else if (auto from = addresses_of({rcv.from}); !from.empty()) {
    e.from = from.front();
  } else if (!fb.from.empty()) {
    e.from = fb.from.front();
  }
  e.to = addresses_of(rcv.to);
  e.cc = addresses_of(rcv.cc);  // bcc is never stored for inbound copies (C2)
  e.reply_to = !hdr.reply_to.empty() ? hdr.reply_to : addresses_of(rcv.reply_to);
  if (e.reply_to.empty()) e.reply_to = fb.reply_to;
  for (const auto& r : rcv.received_for) {
    const std::string_view t = trim(r);
    if (!t.empty()) e.received_for.emplace_back(t);
  }
  if (hdr.subject) e.subject = *hdr.subject;
  else if (!rcv.subject.empty()) e.subject = std::string(trim(mail::eml::decode_rfc2047(rcv.subject)));
  else e.subject = fb.subject.value_or("");
  e.html = rcv.html;
  e.text = rcv.text;
  e.html_format = "cid";
  std::optional<std::string> resend_msgid;
  if (auto n = mail::normalize_message_id(rcv.message_id); !n.empty()) resend_msgid = std::move(n);
  e.message_id = first_of(hdr.message_id, first_of(resend_msgid, fb.message_id));
  e.in_reply_to = first_of(hdr.in_reply_to, fb.in_reply_to);
  e.references = !hdr.references.empty() ? hdr.references : fb.references;
  e.x_azmail_ref = first_of(hdr.x_azmail_ref, fb.x_azmail_ref);
  e.auto_submitted = first_of(hdr.auto_submitted, fb.auto_submitted);
  e.received_at = rcv.created_at_ms;
  e.date = first_of(hdr.date_ms, fb.date_ms).value_or(rcv.created_at_ms);
  e.auth.spf = rcv.auth.spf;
  e.auth.dkim = rcv.auth.dkim;
  e.auth.dmarc = rcv.auth.dmarc;
  e.raw = std::move(raw);
  e.attachments = std::move(attachments);
  e.source = source;
  return e;
}

namespace {

class FetchAttempt {
 public:
  FetchAttempt(Services& svc, const Job& job, std::string resend_id, mail::InboundSource source)
      : svc_(svc), job_(job), id_(std::move(resend_id)), source_(source) {}

  bool failure_recorded = false;

  void fail(std::string_view error) {
    if (failure_recorded) return;
    const int64_t now = svc_.now_ms();
    svc_.db.write([&](db::Tx& tx) { mail::mark_inbound_failed(tx, id_, error, now); });
    failure_recorded = true;
  }

  void run() {
    if (svc_.resend == nullptr) throw Retry(5min, "resend client not configured");
    resend::Client& client = *svc_.resend;
    const auto state = svc_.db.read([&](db::Conn& c) { return mail::inbound_state(c, id_); });
    if (state == mail::InboundState::Delivered || state == mail::InboundState::Unroutable) return;  // idempotent

    resend::ReceivedEmail rcv;
    try {
      rcv = client.get_received(id_);
    } catch (const Error& e) {
      if (e.kind == Error::Kind::NotFound) {
        if (job_.attempts <= static_cast<int>(std::size(kNotFoundDelays)))
          throw Retry(kNotFoundDelays[job_.attempts - 1], "receiving email not found yet");
        fail("not_found: Resend has no received email with this id");
        throw Permanent("receiving email not found");
      }
      map_error(e, "get_received");
    }

    StagedFiles staged;
    // 1. Raw .eml → tmp (hashed while downloading).
    std::optional<fs::path> raw_path;
    std::string raw_sha;
    if (rcv.raw_download_url) {
      const fs::path tmp = make_staging_path(svc_.blobs.tmp_dir());
      staged.paths.push_back(tmp);
      if (auto dl = download(client, *rcv.raw_download_url, tmp, svc_.cfg.inbound_raw_limit, "raw")) {
        raw_path = tmp;
        raw_sha = dl->sha256;
      }
    }
    // 2. Header block (first 512 KB, unfolded) — the raw wins over Resend's fields.
    mail::eml::ParsedHeaders hdr;
    if (raw_path) {
      try {
        hdr = mail::eml::parse_headers(mail::eml::read_file_prefix(*raw_path));
      } catch (const std::exception& e) {
        log::warn("cannot read raw headers", {{"resend_id", id_}, {"error", e.what()}});
      }
    }
    // 3. Attachments: always re-listed for fresh download URLs (they expire after ~1 h, B7).
    std::vector<resend::RecvAttachment> listed;
    try {
      listed = client.list_received_attachments(id_);
    } catch (const Error& e) {
      if (e.kind != Error::Kind::NotFound) map_error(e, "list_received_attachments");
    }
    struct Downloaded {
      const resend::RecvAttachment* meta;
      fs::path path;
      std::string sha;
    };
    std::vector<Downloaded> files;
    for (const auto& att : listed) {
      if (att.download_url.empty()) {
        log::warn("attachment without download URL skipped", {{"resend_id", id_}, {"attachment", att.id}});
        continue;
      }
      const fs::path tmp = make_staging_path(svc_.blobs.tmp_dir());
      staged.paths.push_back(tmp);
      if (auto dl = download(client, att.download_url, tmp, svc_.cfg.inbound_attachment_limit, "attachment"))
        files.push_back({&att, tmp, dl->sha256});
    }

    // 4. Store blobs and deliver, under one writer guard held until COMMIT.
    try {
      auto guard = blob_writer_guard();
      std::optional<BlobRef> raw_ref;
      if (raw_path) raw_ref = svc_.blobs.put_file(*raw_path, sha_or_none(raw_sha));
      std::vector<mail::InboundAttachment> atts;
      for (const auto& f : files) {
        mail::InboundAttachment a;
        a.resend_attachment_id = f.meta->id;
        a.blob = svc_.blobs.put_file(f.path, sha_or_none(f.sha));
        a.filename = f.meta->filename;
        a.content_type = f.meta->content_type;
        a.disposition = f.meta->content_disposition.empty() ? "attachment" : f.meta->content_disposition;
        a.content_id = f.meta->content_id;
        atts.push_back(std::move(a));
      }
      mail::InboundEmail email = build_inbound_email(rcv, hdr, raw_ref, std::move(atts), source_);
      const int64_t now = svc_.now_ms();
      if (email.received_at == 0) email.received_at = now;
      if (email.date == 0) email.date = email.received_at;
      const mail::DeliveryOptions opts{.unroutable_to_admins = svc_.cfg.unroutable_to_admins, .now_ms = now};
      const auto result = svc_.db.write([&](db::Tx& tx) { return mail::deliver_inbound(tx, email, opts); });
      log::info("inbound fetched", {{"resend_id", id_},
                                    {"state", result.state == mail::DeliveryResult::State::Delivered    ? "delivered"
                                              : result.state == mail::DeliveryResult::State::Unroutable ? "unroutable"
                                                                                                        : "duplicate"},
                                    {"copies", static_cast<int64_t>(result.copies.size())},
                                    {"attachments", static_cast<int64_t>(files.size())}});
    } catch (const BlobError& e) {
      if (!e.retryable) {
        fail(std::string("storage: ") + e.what());
        throw Permanent(std::string("storage: ") + e.what());
      }
      throw Retry(std::chrono::milliseconds(0), std::string("storage: ") + e.what());
    }
  }

 private:
  static std::optional<std::string> sha_or_none(const std::string& s) {
    if (is_sha256_hex(s)) return s;
    return std::nullopt;
  }

  // Renews the lease before a long transfer; a lost lease means another worker owns the job.
  void ensure_lease() {
    const int64_t until = svc_.now_ms() + kTransferLeaseMs;
    if (!svc_.db.write([&](db::Tx& tx) { return extend_lease(tx, job_.id, job_.attempts, until); }))
      throw Retry(1s, "lease lost", false);
  }

  // Downloads `url` to `dest`; nullopt (logged) when the file exceeds `limit`.
  std::optional<resend::DownloadResult> download(resend::Client& client, const std::string& url, const fs::path& dest,
                                                 std::size_t limit, std::string_view what) {
    ensure_lease();
    try {
      return client.download_to(url, dest, limit);
    } catch (const Error& e) {
      if (e.kind == Error::Kind::Validation && e.name == "too_large") {
        log::warn("inbound part over the size limit skipped",
                  {{"resend_id", id_}, {"part", what}, {"limit", static_cast<int64_t>(limit)}});
        return std::nullopt;
      }
      map_error(e, "download");
    }
  }

  [[noreturn]] void map_error(const Error& e, std::string_view op) {
    if (e.kind == Error::Kind::RateLimited) throw Retry(retry_after(e), "rate_limited", false);
    if (e.kind == Error::Kind::Network && e.name == "stopped") throw Retry(1s, "shutdown", false);
    if (e.kind == Error::Kind::Auth || e.kind == Error::Kind::Validation) {
      fail(std::string(op) + ": " + error_name(e));
      throw Permanent(std::string(op) + ": " + error_name(e));
    }
    throw Retry(std::chrono::milliseconds(0), std::string(op) + ": " + error_name(e));
  }

  Services& svc_;
  const Job& job_;
  std::string id_;
  mail::InboundSource source_;
};

}  // namespace

void run_inbound_fetch(Services& svc, const Job& job, std::stop_token) {
  std::string resend_id;
  if (auto it = job.payload.find(payload::kResendId); it != job.payload.end() && it->value().is_string())
    resend_id = std::string(it->value().as_string());
  if (resend_id.empty()) throw Permanent("payload has no resend_id");
  mail::InboundSource source = mail::InboundSource::Webhook;
  if (auto it = job.payload.find(payload::kSource); it != job.payload.end() && it->value().is_string())
    source = mail::parse_inbound_source(it->value().as_string()).value_or(mail::InboundSource::Webhook);

  const bool last = job.attempts >= job.max_attempts;
  FetchAttempt attempt(svc, job, resend_id, source);
  auto record_last = [&](std::string_view why) {
    try {
      attempt.fail(std::string("gave up after ") + std::to_string(job.attempts) + " attempts: " + std::string(why));
    } catch (const std::exception& e) {
      log::error("cannot mark inbound failed", {{"resend_id", resend_id}, {"error", e.what()}});
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
    if (last) record_last(e.what());
    throw;
  }
}

void run_poll_receiving(Services& svc, const Job&, std::stop_token st) {
  if (svc.resend == nullptr) return;
  resend::Client& client = *svc.resend;
  const auto high_water = svc.db.read([&](db::Conn& c) { return db::kv_get(c, db::kv_keys::kPollHighWater); });

  std::vector<std::string> unknown;
  std::unordered_set<std::string> seen;
  std::optional<std::string> newest;
  std::optional<std::string> cursor;
  bool found_known = false, passed_high_water = false, end_reached = false, page_limit = false;
  int gap = 0;
  for (int page = 0; page < kPollMaxPages; ++page) {
    if (st.stop_requested()) break;
    resend::ReceivedPage p;
    try {
      p = client.list_received(kPollPageSize, cursor, std::nullopt);
    } catch (const Error& e) {
      if (page == 0) {
        if (e.kind == Error::Kind::RateLimited) throw Retry(retry_after(e), "rate_limited", false);
        throw Retry(std::chrono::milliseconds(0), "list_received: " + error_name(e));
      }
      log::warn("poll stopped early", {{"page", page}, {"error", error_name(e)}});
      break;
    }
    std::vector<std::string> fresh;
    for (const auto& id : p.ids)
      if (!id.empty() && seen.insert(id).second) fresh.push_back(id);
    if (fresh.empty()) {  // cursor did not advance (B8: direction is never trusted)
      end_reached = !p.has_more;
      break;
    }
    if (!newest) newest = fresh.front();
    const auto states = svc.db.read([&](db::Conn& c) {
      std::vector<bool> known;
      for (const auto& id : fresh) known.push_back(mail::inbound_state(c, id).has_value());
      return known;
    });
    for (std::size_t i = 0; i < fresh.size(); ++i) {
      if (high_water && fresh[i] == *high_water) passed_high_water = true;
      if (states[i]) {
        found_known = true;
        continue;
      }
      unknown.push_back(fresh[i]);
      if (passed_high_water) ++gap;  // older than the high-water mark, yet never seen (B7)
    }
    if (found_known) break;
    if (!p.has_more) {
      end_reached = true;
      break;
    }
    cursor = fresh.back();
    if (page == kPollMaxPages - 1) page_limit = true;
  }

  const int64_t now = svc.now_ms();
  std::optional<std::string> warning;
  if (gap > 0) {
    warning = iso8601_utc(now) + " 发现 " + std::to_string(gap) + " 封早于上次同步位置但从未收到的邮件（已补收）";
  } else if (high_water && !found_known && !unknown.empty() && (page_limit || end_reached)) {
    warning = iso8601_utc(now) +
              " 未找到上次同步位置（可能超过 Resend 30 天保留期或超过 20 页），期间的邮件可能已丢失";
  }
  svc.db.write([&](db::Tx& tx) {
    for (const auto& id : unknown) {
      mail::record_inbound_pending(tx, id, mail::InboundSource::Poll, now);
      enqueue(tx, kinds::kInboundFetch, {{std::string(payload::kResendId), id}, {std::string(payload::kSource), "poll"}},
              {.dedupe_key = dedupe_inbound_fetch(id), .priority = kPriorityNormal, .now_ms = now});
    }
    db::kv_set_i64(tx, db::kv_keys::kLastPollAt, now, now);
    if (newest) db::kv_set(tx, db::kv_keys::kPollHighWater, *newest, now);
    if (warning) db::kv_set(tx, db::kv_keys::kPollGapWarning, *warning, now);
  });
  if (!unknown.empty()) log::info("poll found new inbound mail", {{"count", static_cast<int64_t>(unknown.size())}});
  if (warning) log::warn("poll gap detected", {{"gap", gap}});
}

void register_inbound_jobs(Runner& runner) {
  const Config& cfg = runner.services().cfg;
  runner.on(std::string(kinds::kInboundFetch), std::string(lanes::kInbound), run_inbound_fetch);
  runner.on(std::string(kinds::kPollReceiving), std::string(lanes::kSync), run_poll_receiving,
            std::chrono::seconds(std::max(10, cfg.poll_interval_sec)));
}

}  // namespace azm::jobs
