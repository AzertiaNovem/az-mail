// Owner: WP0
// Compile check for every contract header plus assertions on the trivial inline helpers that
// WP0 implemented (enum spellings, status precedence, lanes, WS payloads, http helpers, route
// table invariants, Services wiring, run_blocking). Stubs are expected to throw NotImplemented.
#include "api/dto.hpp"
#include "api/handlers.hpp"
#include "api/routes.hpp"
#include "app/app.hpp"
#include "app/cli.hpp"
#include "config.hpp"
#include "core/blob_store.hpp"
#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "core/signed_url.hpp"
#include "core/time.hpp"
#include "db/kv.hpp"
#include "db/migrations.hpp"
#include "db/sqlite.hpp"
#include "http/blocking.hpp"
#include "http/cors.hpp"
#include "http/dispatch.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "http/session.hpp"
#include "http/throttle.hpp"
#include "http/types.hpp"
#include "jobs/handlers.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "jobs/webhook_dispatch.hpp"
#include "mail/attachments.hpp"
#include "mail/drafts.hpp"
#include "mail/eml.hpp"
#include "mail/fts.hpp"
#include "mail/html_text.hpp"
#include "mail/inbound.hpp"
#include "mail/mailbox.hpp"
#include "mail/outbound.hpp"
#include "mail/render.hpp"
#include "mail/search.hpp"
#include "mail/serde.hpp"
#include "mail/threads.hpp"
#include "mail/types.hpp"
#include "net/http_client.hpp"
#include "notifier.hpp"
#include "repo/accounts.hpp"
#include "resend/client.hpp"
#include "resend/rate_limiter.hpp"
#include "resend/svix.hpp"
#include "resend/types.hpp"
#include "services.hpp"
#include "storage/file_cache.hpp"
#include "storage/r2_blob_store.hpp"
#include "storage/s3_sigv4.hpp"
#include "test_support.hpp"
#include "ws/events.hpp"
#include "ws/hub.hpp"
#include "ws/ws_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json/parse.hpp>

#include <cstdlib>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>

using namespace azm;
namespace json = boost::json;
using boost::beast::http::verb;

TEST_CASE("mail enums: wire spelling round-trips", "[contracts][mail]") {
  for (auto f : mail::kAllFolders) CHECK(mail::parse_folder(mail::to_string(f)) == f);
  CHECK(mail::parse_folder("inbox") == mail::Folder::Inbox);
  CHECK(mail::parse_folder("drafts") == mail::Folder::Drafts);
  CHECK_FALSE(mail::parse_folder("Inbox").has_value());
  CHECK_FALSE(mail::parse_folder("").has_value());
  CHECK_FALSE(mail::parse_folder("label").has_value());

  for (auto s : mail::kAllOutboundStatuses) CHECK(mail::parse_outbound_status(mail::to_string(s)) == s);
  CHECK(mail::to_string(mail::OutboundStatus::DeliveryDelayed) == "delivery_delayed");
  CHECK_FALSE(mail::parse_outbound_status("cancelled").has_value());

  for (auto a : mail::kAllThreadActions) CHECK(mail::parse_thread_action(mail::to_string(a)) == a);
  CHECK(mail::parse_thread_action("not_spam") == mail::ThreadAction::NotSpam);
  CHECK(mail::parse_thread_action("delete_forever") == mail::ThreadAction::DeleteForever);
  CHECK_FALSE(mail::parse_thread_action("move").has_value());
  CHECK(mail::needs_label(mail::ThreadAction::AddLabel));
  CHECK(mail::needs_label(mail::ThreadAction::RemoveLabel));
  CHECK_FALSE(mail::needs_label(mail::ThreadAction::Archive));

  for (auto m : {mail::DraftMode::New, mail::DraftMode::Reply, mail::DraftMode::ReplyAll,
                 mail::DraftMode::Forward})
    CHECK(mail::parse_draft_mode(mail::to_string(m)) == m);
  CHECK(mail::to_string(mail::DraftMode::ReplyAll) == "reply_all");
  CHECK(mail::parse_direction("out") == mail::Direction::Out);
  CHECK(mail::parse_scheduled_via("local") == mail::ScheduledVia::Local);
  CHECK(mail::parse_inbound_source("poll") == mail::InboundSource::Poll);
  CHECK(mail::parse_inbound_state("unroutable") == mail::InboundState::Unroutable);
  CHECK(mail::to_string(mail::ContactKind::Team) == "team");

  // Usable at compile time.
  static_assert(mail::parse_folder("spam") == mail::Folder::Spam);
  static_assert(mail::to_string(mail::Folder::Trash) == "trash");
}

TEST_CASE("OutboundStatus precedence (DESIGN B3)", "[contracts][mail]") {
  using S = mail::OutboundStatus;
  using mail::rank;
  CHECK(rank(S::Queued) < rank(S::Sending));
  CHECK(rank(S::Sending) < rank(S::Accepted));
  CHECK(rank(S::Accepted) == rank(S::Scheduled));
  CHECK(rank(S::Scheduled) < rank(S::Sent));
  CHECK(rank(S::Sent) < rank(S::DeliveryDelayed));
  CHECK(rank(S::DeliveryDelayed) < rank(S::Delivered));
  CHECK(rank(S::Delivered) < rank(S::Complained));
  CHECK(rank(S::Complained) < rank(S::Bounced));
  CHECK(rank(S::Bounced) == rank(S::Failed));
  CHECK(rank(S::Failed) == rank(S::Suppressed));
  CHECK(rank(S::Suppressed) < rank(S::Canceled));

  CHECK(mail::should_apply(S::Sent, S::Delivered));
  CHECK(mail::should_apply(S::DeliveryDelayed, S::Delivered));
  CHECK(mail::should_apply(S::Delivered, S::Bounced));
  CHECK_FALSE(mail::should_apply(S::Delivered, S::DeliveryDelayed));  // shuffled events
  CHECK_FALSE(mail::should_apply(S::Delivered, S::Sent));
  CHECK_FALSE(mail::should_apply(S::Accepted, S::Scheduled));         // equal rank
  CHECK_FALSE(mail::should_apply(S::Bounced, S::Failed));
  CHECK_FALSE(mail::should_apply(S::Canceled, S::Sent));              // local terminal
  CHECK_FALSE(mail::should_apply(S::Sent, S::Sent));

  CHECK(mail::is_terminal(S::Bounced));
  CHECK(mail::is_terminal(S::Canceled));
  CHECK_FALSE(mail::is_terminal(S::Delivered));

  CHECK(mail::status_for_event("email.delivered") == S::Delivered);
  CHECK(mail::status_for_event("email.delivery_delayed") == S::DeliveryDelayed);
  CHECK(mail::status_for_event("email.scheduled") == S::Scheduled);
  CHECK(mail::status_for_event("local.canceled") == S::Canceled);
  CHECK_FALSE(mail::status_for_event("email.opened").has_value());
  CHECK_FALSE(mail::status_for_event("email.clicked").has_value());
  CHECK(mail::event_type_for_last_event("bounced") == "email.bounced");
}

TEST_CASE("normalize_message_id", "[contracts][mail]") {
  CHECK(mail::normalize_message_id("<abc@example.com>") == "abc@example.com");
  CHECK(mail::normalize_message_id("  < abc@x >\r\n") == "abc@x");
  CHECK(mail::normalize_message_id("abc@x") == "abc@x");
  CHECK(mail::normalize_message_id("<>").empty());
  CHECK(mail::normalize_message_id("").empty());
  CHECK(mail::normalize_message_id("<a@b") == "<a@b");
}

TEST_CASE("job kinds, lanes and dedupe keys (DESIGN §7)", "[contracts][jobs]") {
  using namespace jobs;
  CHECK(lane_for(kinds::kOutboundSend) == lanes::kOutbound);
  CHECK(lane_for(kinds::kOutboundFetchMeta) == lanes::kSync);
  CHECK(lane_for(kinds::kOutboundReconcile) == lanes::kMaintenance);
  CHECK(lane_for(kinds::kInboundFetch) == lanes::kInbound);
  CHECK(lane_for(kinds::kPollReceiving) == lanes::kSync);
  for (auto k : {kinds::kPurgeTrash, kinds::kGcBlobs, kinds::kGcHousekeeping, kinds::kDbOptimize})
    CHECK(lane_for(k) == lanes::kMaintenance);
  CHECK_FALSE(lane_for("outbound.unknown").has_value());

  CHECK(dedupe_outbound_send(42) == "out:send:42");
  CHECK(dedupe_fetch_meta(7) == "out:meta:7");
  CHECK(dedupe_inbound_fetch("re_123") == "in:re_123");
  CHECK(dedupe_periodic(kinds::kGcBlobs) == "periodic:gc.blobs");
  CHECK(kPriorityHigh > kPriorityNormal);
  CHECK(kPriorityNormal > kPriorityLow);

  Retry r(std::chrono::seconds(2), "in flight", false);
  CHECK(r.delay == std::chrono::milliseconds(2000));
  CHECK_FALSE(r.count_attempt);
  CHECK(std::string(r.what()) == "in flight");
  CHECK(Retry{}.count_attempt);
  CHECK(std::string(Permanent("quota").what()) == "quota");

  RunnerConfig rc;
  CHECK(rc.lane_threads.at("outbound") == 2);
  CHECK(rc.lane_threads.size() == 4);

  CHECK(to_string(WebhookResult::IgnoredUnknown) == "ignored_unknown");
  CHECK(to_string(WebhookResult::Enqueued) == "enqueued");
}

TEST_CASE("WS event names and flat payloads (API.md)", "[contracts][ws]") {
  CHECK(ws::events::kMailNew == "mail.new");
  CHECK(ws::events::kThreadsChanged == "threads.changed");
  CHECK(ws::events::kOutboundStatus == "outbound.status");
  CHECK(ws::events::kSessionRevoked == "session.revoked");
  CHECK(ws::kCloseAuthFailed == 4401);

  const int64_t ids[] = {3, 5};
  auto tc = ws::threads_changed_payload(ids);
  CHECK(tc.at("thread_ids").as_array().size() == 2);

  auto mn = ws::mail_new_payload(1, 2, Address{"张三", "a@b.cn"}, "周报", "摘要", true, false);
  CHECK(mn.at("from").as_object().at("name").as_string() == "张三");
  CHECK(mn.at("in_inbox").as_bool());

  auto os = ws::outbound_status_payload(10, 11, 12, "delivered", std::nullopt);
  CHECK(os.at("status_detail").is_null());
  CHECK(os.at("outbound_id").as_int64() == 12);

  auto frame = json::parse(ws::make_frame(ws::events::kOutboundStatus, os)).as_object();
  CHECK(frame.at("type").as_string() == "outbound.status");
  CHECK(frame.at("message_id").as_int64() == 10);
  CHECK_FALSE(frame.contains("data"));
  CHECK(json::parse(ws::make_frame(ws::events::kLabelsChanged, {})).as_object().size() == 1);
}

TEST_CASE("http types: Request::make, Ctx helpers, Response factories", "[contracts][http]") {
  auto req = http::Request::make(verb::get, "/api/threads?folder=inbox&limit=20&q=%E5%91%A8+a%2Bb&folder=x");
  CHECK(req.path == "/api/threads");
  CHECK(req.query.at("folder") == "inbox");  // first occurrence wins
  CHECK(req.query.at("q") == "周+a+b");       // '+' is not a space
  CHECK_THROWS_AS(http::Request::make(verb::get, "not a target"), std::invalid_argument);
  req.headers.set("Authorization", "Bearer tok");
  CHECK(req.header("authorization") == std::optional<std::string_view>("Bearer tok"));
  CHECK_FALSE(req.header("x-missing").has_value());

  Config cfg;
  test::TempDir td;
  db::Pool pool(td / "c.db", 1);
  auto blobs = make_local_blob_store(td.path());
  SignedUrls urls("secret-secret-secret-secret-secret", cfg.public_api_base_url);
  RecordingNotifier rec;
  ManualClock clock(1'700'000'000'000);
  Services svc{cfg, pool, *blobs, urls, rec, clock};

  http::Ctx ctx{req, svc, {{"id", "42"}, {"bad", "4x"}, {"neg", "-1"}}, std::nullopt};
  CHECK(ctx.id("id") == 42);
  CHECK(ctx.query("folder") == std::optional<std::string_view>("inbox"));
  CHECK(ctx.query_int("limit") == 20);
  CHECK_FALSE(ctx.query_int("cursor").has_value());
  try {
    (void)ctx.id("bad");
    FAIL("expected ApiError");
  } catch (const ApiError& e) {
    CHECK(e.status == 400);
    CHECK(e.code == "invalid_field");
    CHECK(e.details.at("field").as_string() == "bad");
  }
  CHECK_THROWS_AS(ctx.id("neg"), ApiError);
  CHECK_THROWS_AS(ctx.id("missing"), ApiError);
  CHECK_THROWS_AS(ctx.query_int("folder"), ApiError);
  try {
    (void)ctx.user();
    FAIL("expected 401");
  } catch (const ApiError& e) {
    CHECK(e.status == 401);
  }
  try {
    (void)ctx.body_object();
    FAIL("expected invalid_json");
  } catch (const ApiError& e) {
    CHECK(e.code == "invalid_json");
  }
  ctx.principal = http::Principal{7, 9, true, "a@b.cn"};
  CHECK(ctx.user().user_id == 7);

  auto r = http::Response::json(json::object{{"ok", true}}, 201);
  CHECK(r.status == 201);
  CHECK(std::get<std::string>(r.body) == R"({"ok":true})");
  auto e = http::Response::error(409, "version_conflict", "已在其他窗口修改", {{"current", 1}});
  auto eo = json::parse(std::get<std::string>(e.body)).as_object();
  CHECK(eo.at("error").at("code").as_string() == "version_conflict");
  CHECK(eo.at("error").at("details").at("current").as_int64() == 1);
  CHECK(http::Response::no_content().status == 204);
  auto f = http::Response::file("/tmp/x", "image/png", "inline");
  CHECK(std::holds_alternative<http::FileRef>(f.body));
  CHECK(f.find_header("content-disposition") == std::optional<std::string_view>("inline"));
  auto rd = http::Response::redirect("https://r2.example/x");
  CHECK(rd.status == 302);
  CHECK(rd.find_header("Location").has_value());
  CHECK(http::to_string(http::Exec::Db) == "db");
  CHECK(http::to_string(http::Exec::Net) == "net");
  CHECK(http::to_string(http::Exec::Files) == "files");
}

TEST_CASE("run_blocking runs on the pool and resumes on the caller", "[contracts][http]") {
  boost::asio::thread_pool pool(2);
  boost::asio::io_context ioc;
  const auto main_id = std::this_thread::get_id();
  std::thread::id worker, resumed;
  int value = 0;
  bool caught = false, void_ran = false;
  boost::asio::co_spawn(
      ioc,
      [&]() -> boost::asio::awaitable<void> {
        value = co_await http::run_blocking(pool, [&] {
          worker = std::this_thread::get_id();
          return 41 + 1;
        });
        resumed = std::this_thread::get_id();
        co_await http::run_blocking(pool, [&] { void_ran = true; });
        try {
          co_await http::run_blocking(pool, []() -> int { throw ApiError::conflict(); });
        } catch (const ApiError& e) {
          caught = e.status == 409;
        }
      },
      boost::asio::detached);
  ioc.run();
  pool.join();
  CHECK(value == 42);
  CHECK(worker != main_id);
  CHECK(resumed == main_id);
  CHECK(void_ran);
  CHECK(caught);
}

TEST_CASE("route table invariants (docs/CONTRACTS.md §B)", "[contracts][api]") {
  const auto routes = api::route_table();
  CHECK(routes.size() == 56);
  std::set<std::pair<int, std::string>> seen;
  for (const auto& r : routes) {
    INFO(r.pattern);
    CHECK(r.pattern.rfind("/api/", 0) == 0);
    CHECK(static_cast<bool>(r.handler));
    CHECK(seen.emplace(static_cast<int>(r.method), r.pattern).second);  // unique (method, pattern)
    if (r.body == http::BodyMode::Json || r.body == http::BodyMode::Raw || r.body == http::BodyMode::File)
      CHECK(r.body_limit > 0);
    if (r.method == verb::get) CHECK(r.body_limit == 0);
    if (r.pattern.rfind("/api/admin/", 0) == 0) CHECK(r.auth == http::AuthReq::Admin);
    if (r.pattern.rfind("/api/files/", 0) == 0) CHECK(r.auth == http::AuthReq::Signed);
  }
  auto find = [&](verb m, std::string_view p) -> const http::Route* {
    for (const auto& r : routes)
      if (r.method == m && r.pattern == p) return &r;
    return nullptr;
  };
  const auto* login = find(verb::post, "/api/auth/login");
  REQUIRE(login);
  CHECK(login->auth == http::AuthReq::None);
  const auto* hook = find(verb::post, "/api/webhooks/resend");
  REQUIRE(hook);
  CHECK(hook->auth == http::AuthReq::Webhook);
  CHECK(hook->body == http::BodyMode::Raw);
  CHECK(hook->body_limit == (1u << 20));
  const auto* up = find(verb::post, "/api/attachments");
  REQUIRE(up);
  CHECK(up->body == http::BodyMode::File);
  CHECK(up->body_limit == (25u << 20));
  CHECK(up->exec == http::Exec::Files);
  // File I/O (possibly synchronous R2 GET/PUT) runs on its own pool, never on net/db.
  for (auto [m, p] : {std::pair{verb::get, "/api/files/:id"}, std::pair{verb::get, "/api/files/raw/:messageId"},
                      std::pair{verb::get, "/api/messages/:id/raw"}}) {
    const auto* r = find(m, p);
    REQUIRE(r);
    CHECK(r->exec == http::Exec::Files);
  }
  int files_routes = 0;
  for (const auto& r : routes) files_routes += r.exec == http::Exec::Files ? 1 : 0;
  CHECK(files_routes == 4);
  const auto* send = find(verb::post, "/api/drafts/:id/send");
  REQUIRE(send);
  CHECK(send->body_limit == (8u << 20));
  for (auto p : {"/api/messages/:id/cancel-schedule", "/api/messages/:id/reschedule"}) {
    const auto* r = find(verb::post, p);
    REQUIRE(r);
    CHECK(r->exec == http::Exec::Net);
  }
  const auto* st = find(verb::get, "/api/admin/domains/:id/status");
  REQUIRE(st);
  CHECK(st->exec == http::Exec::Net);
  CHECK(find(verb::get, "/api/health")->auth == http::AuthReq::None);

  Config cfg;
  cfg.upload_body_limit = 5u << 20;
  CHECK(api::route_limits_from(cfg).upload == (5u << 20));
  bool found = false;
  for (const auto& r : api::route_table(api::route_limits_from(cfg)))
    if (r.pattern == "/api/attachments") found = r.body_limit == (5u << 20);
  CHECK(found);
}

namespace {

// Test doubles must be constructible through the protected default constructors.
struct FakeHttp final : net::HttpClient {
  net::HttpResponse send(const net::HttpRequest& req) override {
    net::HttpResponse r;
    r.status = 200;
    r.final_url = req.url;
    r.headers = {{"content-type", "text/plain"}};
    return r;
  }
};

struct FakeResend final : resend::Client {
  std::string send(const resend::SendRequest& req) override { return "re_" + req.idempotency_key; }
};

struct FakeSink final : ws::WsSink {
  void send(std::shared_ptr<const std::string>) override {}
  void close(std::uint16_t, std::string_view) override {}
};

}  // namespace

TEST_CASE("Services wiring and test doubles", "[contracts][services]") {
  Config cfg;
  test::TempDir td;
  db::Pool pool(td / "s.db", 2);
  auto local = make_local_blob_store(td / "local");
  auto other = make_local_blob_store(td / "other");
  SignedUrls urls("0123456789abcdef0123456789abcdef", "http://127.0.0.1:8080");
  RecordingNotifier rec;
  ManualClock clock(123);
  Services svc{cfg, pool, *local, urls, rec, clock};

  CHECK(svc.now_ms() == 123);
  CHECK(&svc.blobs_for("local") == local.get());
  // No store of that kind → BlobError{retryable=false}, never a silent fallback.
  try {
    (void)svc.blobs_for("r2");
    FAIL("expected BlobError");
  } catch (const BlobError& e) {
    CHECK_FALSE(e.retryable);
  }
  CHECK_THROWS_AS(svc.blobs_for(""), BlobError);
  // A secondary of the same kind is only consulted when the primary does not match.
  svc.secondary_blobs = other.get();
  CHECK(&svc.blobs_for("local") == local.get());
  CHECK_THROWS_AS(svc.blobs_for("r2"), BlobError);
  svc.secondary_blobs = nullptr;
  try {
    (void)svc.resend_client();
    FAIL("expected 503");
  } catch (const ApiError& e) {
    CHECK(e.status == 503);
    CHECK(e.code == "service_unavailable");
  }

  FakeResend fr;
  svc.resend = &fr;
  resend::SendRequest sr;
  sr.idempotency_key = "u1";
  CHECK(svc.resend_client().send(sr) == "re_u1");
  CHECK_THROWS_AS(svc.resend_client().cancel("x"), NotImplemented);

  FakeHttp fh;
  net::HttpClient& base = fh;
  net::HttpRequest hreq;
  hreq.url = "https://example.com/";
  auto resp = base.send(hreq);
  CHECK(resp.status == 200);
  CHECK(resp.header("Content-Type") == std::optional<std::string>("text/plain"));
  CHECK_FALSE(resp.header("etag").has_value());

  ws::Hub hub;  // Notifier stub is a harmless no-op
  hub.publish(1, "threads.changed", {});
  Notifier& n = hub;
  n.revoke_user(1);
  [[maybe_unused]] FakeSink sink;

  CHECK(repo::session_token_hash("tok") == crypto::sha256("tok"));
}

TEST_CASE("Resend / net / svix inline helpers", "[contracts][resend]") {
  using K = resend::Error::Kind;
  CHECK(resend::Error(K::RateLimited, 429, "rate_limit_exceeded", "").retryable());
  CHECK(resend::Error(K::IdempotencyInFlight, 409, "concurrent_idempotent_requests", "").retryable());
  CHECK(resend::Error(K::Network, 0, "", "").retryable());
  CHECK_FALSE(resend::Error(K::Quota, 429, "daily_quota_exceeded", "").retryable());
  CHECK_FALSE(resend::Error(K::IdempotencyConflict, 409, "invalid_idempotent_request", "").retryable());
  CHECK(std::string(resend::Error(K::Validation, 422, "", "").what()) == "resend_error");
  CHECK(resend::to_string(K::IdempotencyInFlight) == "idempotency_in_flight");
  CHECK(resend::to_string(resend::SvixResult::BadTimestamp) == "bad_timestamp");

  resend::WebhookEnvelope env;
  env.type = "email.received";
  CHECK(env.is_received());
  CHECK_FALSE(env.is_outbound_event());
  env.type = "email.bounced";
  CHECK(env.is_outbound_event());
  env.type = "domain.updated";
  CHECK_FALSE(env.is_outbound_event());

  CHECK(net::NetError(net::NetError::Kind::Timeout, "t").retryable());
  CHECK_FALSE(net::NetError(net::NetError::Kind::Tls, "t").retryable());
  CHECK(storage::sigv4::kEmptyPayloadSha256 == crypto::sha256_hex(""));
}

TEST_CASE("db kv helpers", "[contracts][db]") {
  test::TempDir td;
  db::Pool pool(td / "kv.db", 1);
  {
    auto lease = pool.acquire();
    db::migrate(*lease);
  }
  pool.write([](db::Tx& tx) {
    db::kv_set(tx, db::kv_keys::kLastPollAt, "x", 1);
    db::kv_set_i64(tx, db::kv_keys::kLastWebhookAt, 1700000000123, 2);
    db::kv_set_i64(tx, db::kv_keys::kLastWebhookAt, 1700000000456, 3);  // upsert
  });
  pool.read([](db::Conn& c) {
    CHECK(db::kv_get(c, db::kv_keys::kLastPollAt) == std::optional<std::string>("x"));
    CHECK_FALSE(db::kv_get_i64(c, db::kv_keys::kLastPollAt).has_value());  // not an integer
    CHECK(db::kv_get_i64(c, db::kv_keys::kLastWebhookAt) == 1700000000456);
    CHECK_FALSE(db::kv_get(c, db::kv_keys::kQuotaBlocked).has_value());
    CHECK(c.scalar<int64_t>("SELECT updated_at FROM kv WHERE key=?", db::kv_keys::kLastWebhookAt) == 3);
  });
  CHECK(pool.write([](db::Tx& tx) { return db::kv_delete(tx, db::kv_keys::kLastPollAt); }));
  CHECK_FALSE(pool.write([](db::Tx& tx) { return db::kv_delete(tx, db::kv_keys::kLastPollAt); }));
}

TEST_CASE("jobs::enqueue / cancel / reschedule (implemented in WP0 for WP-B)", "[contracts][jobs]") {
  test::TempDir td;
  int wakes = 0;
  db::Pool pool(td / "jobs.db", 1, db::TxHooks{nullptr, [&] { ++wakes; }});
  {
    auto lease = pool.acquire();
    db::migrate(*lease);
  }
  const auto key = jobs::dedupe_outbound_send(7);
  const int64_t a = pool.write([&](db::Tx& tx) {
    return jobs::enqueue(tx, jobs::kinds::kOutboundSend, {{"outbound_id", 7}},
                         {.run_at_ms = 5000, .dedupe_key = key, .max_attempts = 10,
                          .priority = jobs::kPriorityHigh});
  });
  CHECK(wakes == 1);
  // Same dedupe key while pending → same id, nothing inserted.
  const int64_t b = pool.write([&](db::Tx& tx) {
    return jobs::enqueue(tx, jobs::kinds::kOutboundSend, {{"outbound_id", 7}}, {.dedupe_key = key});
  });
  CHECK(a == b);
  pool.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM jobs") == 1);
    auto s = c.prepare("SELECT kind,lane,priority,payload,state,run_at,max_attempts,dedupe_key FROM jobs WHERE id=?");
    s.bind_all(a);
    REQUIRE(s.step());
    CHECK(s.text(0) == "outbound.send");
    CHECK(s.text(1) == "outbound");
    CHECK(s.i64(2) == jobs::kPriorityHigh);
    CHECK(json::parse(s.text(3)).as_object().at("outbound_id").as_int64() == 7);
    CHECK(s.text(4) == "pending");
    CHECK(s.i64(5) == 5000);
    CHECK(s.i64(6) == 10);
    CHECK(s.text(7) == key);
  });
  CHECK(pool.write([&](db::Tx& tx) { return jobs::reschedule(tx, a, 9000); }));
  CHECK(pool.read([&](db::Conn& c) { return *c.scalar<int64_t>("SELECT run_at FROM jobs WHERE id=?", a); }) == 9000);
  CHECK(pool.write([&](db::Tx& tx) { return jobs::cancel(tx, a); }));
  CHECK_FALSE(pool.write([&](db::Tx& tx) { return jobs::cancel(tx, a); }));          // not pending
  CHECK_FALSE(pool.write([&](db::Tx& tx) { return jobs::reschedule(tx, a, 1); }));  // not pending
  // Canceled no longer holds the dedupe key; lane follows the kind; run_at 0 = now.
  const int64_t c2 = pool.write([&](db::Tx& tx) {
    return jobs::enqueue(tx, jobs::kinds::kOutboundSend, {{"outbound_id", 7}}, {.dedupe_key = key});
  });
  CHECK(c2 != a);
  const int64_t d = pool.write([&](db::Tx& tx) {
    return jobs::enqueue(tx, jobs::kinds::kInboundFetch, {{"resend_id", "r1"}, {"source", "poll"}});
  });
  pool.read([&](db::Conn& c) {
    CHECK(c.scalar<std::string>("SELECT lane FROM jobs WHERE id=?", d) == "inbound");
    CHECK(*c.scalar<int64_t>("SELECT run_at FROM jobs WHERE id=?", d) > 1'600'000'000'000);
  });
  CHECK_THROWS_AS(pool.write([&](db::Tx& tx) { return jobs::enqueue(tx, "nope.kind", {}); }),
                  std::invalid_argument);
}

TEST_CASE("jobs: EnqueueOpts::now_ms and extend_lease (WP0)", "[contracts][jobs]") {
  test::TempDir td;
  db::Pool pool(td / "lease.db", 1);
  test::migrate(pool);
  // now_ms drives created_at / updated_at and the default run_at (ManualClock-driven tests).
  const int64_t id = pool.write([&](db::Tx& tx) {
    return jobs::enqueue(tx, jobs::kinds::kInboundFetch, {{"resend_id", "r9"}, {"source", "poll"}},
                         {.dedupe_key = jobs::dedupe_inbound_fetch("r9"), .now_ms = 42});
  });
  pool.read([&](db::Conn& c) {
    auto s = c.prepare("SELECT run_at, created_at, updated_at FROM jobs WHERE id=?");
    s.bind_all(id);
    REQUIRE(s.step());
    CHECK(s.i64(0) == 42);
    CHECK(s.i64(1) == 42);
    CHECK(s.i64(2) == 42);
  });
  // Not running → no lease to extend.
  CHECK_FALSE(pool.write([&](db::Tx& tx) { return jobs::extend_lease(tx, id, 0, 500); }));
  // Simulate the Runner's claim (attempt 1).
  pool.write([&](db::Tx& tx) {
    tx.run("UPDATE jobs SET state='running', attempts=1, locked_until=100 WHERE id=?", id);
  });
  CHECK(pool.write([&](db::Tx& tx) { return jobs::extend_lease(tx, id, 1, 500); }));
  CHECK(pool.write([&](db::Tx& tx) { return jobs::extend_lease(tx, id, 1, 300); }));  // never shortens
  CHECK(pool.read([&](db::Conn& c) {
          return *c.scalar<int64_t>("SELECT locked_until FROM jobs WHERE id=?", id);
        }) == 500);
  // A stale worker (older claim) cannot extend a re-claimed job.
  pool.write([&](db::Tx& tx) { tx.run("UPDATE jobs SET attempts=2 WHERE id=?", id); });
  CHECK_FALSE(pool.write([&](db::Tx& tx) { return jobs::extend_lease(tx, id, 1, 900); }));
  CHECK(pool.write([&](db::Tx& tx) { return jobs::extend_lease(tx, id, 2, 900); }));
}

TEST_CASE("resend::ScopedStopToken installs a thread-local token", "[contracts][resend]") {
  CHECK_FALSE(resend::current_stop_token().stop_possible());
  std::stop_source outer;
  {
    resend::ScopedStopToken a(outer.get_token());
    CHECK(resend::current_stop_token().stop_possible());
    std::stop_source inner;
    {
      resend::ScopedStopToken b(inner.get_token());
      inner.request_stop();
      CHECK(resend::current_stop_token().stop_requested());
    }
    CHECK_FALSE(resend::current_stop_token().stop_requested());  // restored to `outer`
    bool other_thread_has_token = true;
    std::thread([&] { other_thread_has_token = resend::current_stop_token().stop_possible(); }).join();
    CHECK_FALSE(other_thread_has_token);  // thread-local
  }
  CHECK_FALSE(resend::current_stop_token().stop_possible());
}

TEST_CASE("Config defaults that other packages rely on", "[contracts][config]") {
  const Config cfg;
  CHECK(cfg.files_delivery == FilesDelivery::Proxy);  // Addendum A default
  CHECK(enum_name(cfg.files_delivery) == "proxy");
  CHECK(cfg.files_threads >= 1);
  const int job_threads = cfg.jobs_outbound_threads + cfg.jobs_inbound_threads +
                          cfg.jobs_sync_threads + cfg.jobs_maintenance_threads;
  // Every blocking worker can hold a SQLite connection at once.
  CHECK(cfg.db_pool_size >= cfg.db_threads + cfg.net_threads + cfg.files_threads + job_threads);
  CHECK(storage::R2Options{}.delivery == cfg.files_delivery);
}

TEST_CASE("test_support seeding helpers and TestServices", "[contracts][test_support]") {
  test::TestServices ts;
  CHECK(std::abs(ts.svc.now_ms() - azm::now_ms()) < 60'000);  // real time by default
  CHECK(&ts.svc.blobs_for("local") == ts.blobs.get());

  int64_t alice = 0, bob = 0, support = 0;
  std::string token;
  ts.db.write([&](db::Tx& tx) {
    alice = test::seed_user(tx, "Alice@Team.Example", true, "Alice");
    bob = test::seed_user(tx, "bob@team.example");
    support = test::seed_alias(tx, "support@team.example", {{alice, true}, {bob, false}});
    token = test::seed_session(tx, alice);
    tx.emit(alice, "threads.changed", {{"thread_ids", json::array{}}});
  });
  CHECK(ts.notifier.events_of("threads.changed").size() == 1);  // TxHooks → RecordingNotifier
  ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT count(*) FROM domains") == 1);  // get-or-create
    CHECK(c.scalar<std::string>("SELECT email FROM users WHERE id=?", alice) ==
          std::optional<std::string>("alice@team.example"));
    CHECK(c.scalar<int64_t>("SELECT is_admin FROM users WHERE id=?", alice) == 1);
    CHECK(c.scalar<int64_t>("SELECT undo_send_seconds FROM user_settings WHERE user_id=?", bob) == 5);
    CHECK(test::address_id(c, "support@team.example") == support);
    CHECK(test::address_id(c, "nobody@team.example") == 0);
    CHECK(c.scalar<int64_t>("SELECT can_send_as FROM alias_members WHERE alias_id=? AND user_id=?",
                            support, alice) == 1);
    const std::string h = repo::session_token_hash(token);
    const std::vector<uint8_t> hash(h.begin(), h.end());
    CHECK(c.scalar<int64_t>("SELECT user_id FROM sessions WHERE token_hash=?", hash) == alice);
  });

  // Why repo::delete_alias has 409 "alias_in_use": outbound.from_address_id is NOT NULL without
  // an ON DELETE action, so deleting an alias that has sent mail violates the foreign key.
  ts.db.write([&](db::Tx& tx) {
    tx.run(
        "INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, "
        "payload_json, created_at, updated_at) VALUES('u-1', ?, ?, 'failed', 1, '{}', 1, 1)",
        alice, support);
  });
  CHECK_THROWS(ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM addresses WHERE id=?", support); }));
  CHECK(ts.db.read([&](db::Conn& c) { return test::address_id(c, "support@team.example"); }) == support);
}

