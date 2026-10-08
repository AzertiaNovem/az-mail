// Owner: WP-D
// Mailbox endpoints (docs/CONTRACTS.md §B): thin wrappers over mail::* (WP-B). Every call passes
// the caller's user id as `owner` (IDOR, DESIGN D6). Cancel / reschedule talk to Resend on the
// net pool between two short transactions (begin_* → network → finish_*).
#include "api/common.hpp"
#include "api/handlers.hpp"
#include "core/blob_store.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "db/sqlite.hpp"
#include "mail/attachments.hpp"
#include "mail/drafts.hpp"
#include "mail/mailbox.hpp"
#include "mail/outbound.hpp"
#include "mail/serde.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "services.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <variant>

namespace azm::api {

namespace {

constexpr int kMaxTzOffsetMin = 14 * 60;
constexpr std::size_t kMaxQueryBytes = 1024;

[[noreturn]] void invalid_param(std::string_view field) {
  throw ApiError::bad_request("invalid_field", "参数无效", detail::field_detail(field));
}

[[noreturn]] void message_not_found() { throw ApiError::not_found("not_found", "邮件不存在"); }

// Resend rejected a cancel / PATCH of a scheduled email: it is no longer scheduled there.
[[noreturn]] void map_schedule_error(const resend::Error& e, std::string_view op) {
  if (e.kind == resend::Error::Kind::Validation)
    throw ApiError::conflict("already_sent", "邮件已开始发送或已发送，无法修改定时");
  detail::throw_resend_error(e, op);
}

}  // namespace

// GET  /api/threads?folder|label_id|q&cursor&limit&tzoff
http::Response threads_list(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  mail::ThreadQuery q;
  // View selector precedence: q > label_id > folder (default inbox). Every given value is
  // validated even when a higher-precedence selector wins.
  std::optional<mail::Folder> folder;
  if (auto f = detail::query_nonempty(ctx, "folder")) {
    folder = mail::parse_folder(*f);
    if (!folder) invalid_param("folder");
  }
  const auto label_id = ctx.query_int("label_id");
  if (label_id && *label_id <= 0) invalid_param("label_id");
  std::optional<std::string> search;
  if (auto s = ctx.query("q"); s && !trim(*s).empty()) {
    if (s->size() > kMaxQueryBytes) invalid_param("q");
    search = std::string(trim(*s));
  }
  if (search) q.q = search;
  else if (label_id) q.label_id = label_id;
  else q.folder = folder.value_or(mail::Folder::Inbox);

  q.cursor = detail::query_nonempty(ctx, "cursor");
  const auto limit = ctx.query_int("limit");
  if (limit && *limit <= 0) invalid_param("limit");
  if (auto tz = ctx.query_int("tzoff")) {
    if (*tz < -kMaxTzOffsetMin || *tz > kMaxTzOffsetMin) invalid_param("tzoff");
    q.tzoff_min = static_cast<int>(*tz);
  }
  q.now_ms = ctx.svc.now_ms();

  const auto page = ctx.svc.db.read([&](db::Conn& c) {
    mail::ThreadQuery query = q;
    const int64_t want = limit ? *limit : repo::get_settings(c, owner).page_size;
    query.limit = static_cast<int>(std::clamp<int64_t>(want, 1, 100));
    return mail::list_threads(c, owner, query);
  });
  return http::Response::json(mail::to_json(page));
}

// GET  /api/threads/:id
http::Response threads_get(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto t = ctx.svc.db.read(
      [&](db::Conn& c) { return mail::get_thread(c, ctx.svc.signed_urls, owner, id); });
  if (!t) throw ApiError::not_found("not_found", "会话不存在");
  return http::Response::json(mail::to_json(*t));
}

// POST /api/threads/actions → {thread_ids}
http::Response threads_actions(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const auto req = mail::parse_thread_action_request(ctx.body_object());
  const auto ids = ctx.svc.db.write([&](db::Tx& tx) {
    return mail::apply_thread_action(tx, owner, req.thread_ids, req.action, req.label_id);
  });
  return http::Response::json(mail::thread_ids_response(ids));
}

// GET  /api/messages/:id
http::Response messages_get(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto m = ctx.svc.db.read(
      [&](db::Conn& c) { return mail::get_message(c, ctx.svc.signed_urls, owner, id); });
  if (!m) message_not_found();
  return http::Response::json(mail::to_json(*m));
}

// PATCH /api/messages/:id → Message
http::Response messages_patch(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto patch = mail::parse_message_patch(ctx.body_object());
  ctx.svc.db.write([&](db::Tx& tx) { mail::patch_message(tx, owner, id, patch); });
  const auto m = ctx.svc.db.read(
      [&](db::Conn& c) { return mail::get_message(c, ctx.svc.signed_urls, owner, id); });
  if (!m) message_not_found();
  return http::Response::json(mail::to_json(*m));
}

// GET  /api/messages/:id/events → {events: DeliveryEvent[]}
http::Response messages_events(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto ev = ctx.svc.db.read([&](db::Conn& c) { return mail::message_events(c, owner, id); });
  if (!ev) message_not_found();
  return http::Response::json(mail::events_response(*ev));
}

// GET  /api/messages/:id/raw (files pool) → text/plain .eml. Never a redirect: the SPA fetches
// it with a Bearer header and R2 serves no CORS headers.
http::Response messages_raw(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto raw = ctx.svc.db.read([&](db::Conn& c) { return mail::find_raw(c, owner, id); });
  if (!raw) message_not_found();

  constexpr std::string_view kType = "text/plain; charset=utf-8";
  try {
    BlobStore& store = ctx.svc.blobs_for(raw->storage);
    ServeOptions opts{std::string(kType), "", std::chrono::seconds(ctx.svc.cfg.r2_presign_ttl_sec)};
    const ServePlan plan = store.serve(raw->sha256, opts);
    http::Response r;
    if (const auto* local = std::get_if<LocalFile>(&plan)) {
      r = http::Response::file(local->path, std::string(kType), "");
    } else {
      // Redirect-mode R2: stream a private temporary copy instead.
      const auto tmp = make_staging_path(store.tmp_dir());
      try {
        store.get_to_file(raw->sha256, tmp);
      } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw;
      }
      r = http::Response::file(tmp, std::string(kType), "");
      std::get<http::FileRef>(r.body).remove_after_send = true;
    }
    r.add_header("X-Content-Type-Options", "nosniff");
    r.add_header("Content-Security-Policy", "sandbox");
    return r;
  } catch (const BlobNotFound&) {
    throw ApiError::not_found("not_found", "原始邮件文件不存在");
  } catch (const BlobError& e) {
    detail::throw_blob_error(e, "messages_raw");
  }
}

// POST /api/messages/:id/undo-send → {draft}
http::Response messages_undo_send(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto d = ctx.svc.db.write(
      [&](db::Tx& tx) { return mail::undo_send(tx, ctx.svc.signed_urls, owner, id); });
  return http::Response::json(mail::draft_response(d));
}

// POST /api/messages/:id/cancel-schedule (net) → {draft}
http::Response messages_cancel_schedule(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const auto plan = svc.db.write(
      [&](db::Tx& tx) { return mail::begin_cancel_schedule(tx, svc.signed_urls, owner, id); });
  if (!plan.remote) {
    if (!plan.draft) throw ApiError::internal();  // contract: local cancels return the draft
    return http::Response::json(mail::draft_response(*plan.draft));
  }
  try {
    svc.resend_client().cancel(plan.resend_id);  // outside any transaction
  } catch (const resend::Error& e) {
    // Review R10: an earlier cancel may have reached Resend while its response was lost; Resend
    // then refuses this one. If it reports the email as canceled, finish the cancel locally
    // instead of claiming the mail was sent.
    bool already_canceled = false;
    if (e.kind == resend::Error::Kind::Validation) {
      try {
        const auto sent = svc.resend_client().get(plan.resend_id, resend::Priority::High);
        already_canceled = sent.last_event && iequals(trim(*sent.last_event), "canceled");
      } catch (const std::exception&) {  // unknown: report the original rejection
      }
    }
    if (!already_canceled) map_schedule_error(e, "cancel");
  }
  const auto d = svc.db.write([&](db::Tx& tx) {
    return mail::finish_cancel_schedule(tx, svc.signed_urls, owner, plan.outbound_id);
  });
  return http::Response::json(mail::draft_response(d));
}

// POST /api/messages/:id/reschedule (net) {scheduled_at} → Message
http::Response messages_reschedule(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const int64_t at = mail::parse_reschedule(ctx.body_object());
  const int64_t now = svc.now_ms();
  const auto plan = svc.db.write(
      [&](db::Tx& tx) { return mail::begin_reschedule(tx, svc.cfg, owner, id, at, now); });
  if (plan.remote) {
    try {
      svc.resend_client().update_schedule(plan.resend_id, iso8601_utc(plan.scheduled_at));
    } catch (const resend::Error& e) {
      map_schedule_error(e, "update_schedule");
    }
    svc.db.write([&](db::Tx& tx) {
      mail::finish_reschedule(tx, owner, plan.outbound_id, plan.scheduled_at);
    });
  }
  const auto m =
      svc.db.read([&](db::Conn& c) { return mail::get_message(c, svc.signed_urls, owner, id); });
  if (!m) message_not_found();
  return http::Response::json(mail::to_json(*m));
}

// POST /api/messages/:id/retry → 202 SendResult
http::Response messages_retry(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const int64_t id = ctx.id("id");
  const int64_t now = ctx.svc.now_ms();
  const auto r = ctx.svc.db.write(
      [&](db::Tx& tx) { return mail::retry_failed_send(tx, owner, id, now); });
  return http::Response::json(mail::to_json(r), 202);
}

// GET  /api/counts
http::Response counts_get(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  const auto c = ctx.svc.db.read([&](db::Conn& conn) { return mail::counts(conn, owner); });
  return http::Response::json(mail::to_json(c));
}

// GET  /api/contacts?q&limit=8 → {items}
http::Response contacts_search(http::Ctx& ctx) {
  const int64_t owner = ctx.user().user_id;
  std::string q(trim(ctx.query("q").value_or("")));
  if (q.size() > 256) q = utf8_truncate(q, 256);
  const auto limit = ctx.query_int("limit").value_or(8);
  const int lim = static_cast<int>(std::clamp<int64_t>(limit, 1, 50));
  const auto items =
      ctx.svc.db.read([&](db::Conn& c) { return mail::search_contacts(c, owner, q, lim); });
  return http::Response::json(mail::contacts_response(items));
}

}  // namespace azm::api
