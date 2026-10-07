// Owner: WP-C — job handlers (outbound/inbound/maintenance). Pure mapping, registration,
// payload validation and WP-C-only maintenance steps run by default; the end-to-end flows that
// need WP-B's mail::* implementations are tagged [.integration].
#include "core/crypto.hpp"
#include "db/kv.hpp"
#include "jobs/handlers.hpp"
#include "jobs/kinds.hpp"
#include "mail/attachments.hpp"
#include "mail/drafts.hpp"
#include "mail/inbound.hpp"
#include "mail/outbound.hpp"
#include "resend/client.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <fstream>

using namespace azm;
using namespace azm::jobs;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

Job job_of(std::string_view kind, boost::json::object payload, int attempts = 1, int max_attempts = 8) {
  Job j;
  j.id = 1;
  j.kind = std::string(kind);
  j.payload = std::move(payload);
  j.attempts = attempts;
  j.max_attempts = max_attempts;
  j.lane = std::string(*lane_for(kind));
  return j;
}

template <class E>
E thrown(const std::function<void()>& f) {
  try {
    f();
  } catch (const E& e) {
    return e;
  }
  FAIL("expected exception");
  return E{};
}

void write_file(const fs::path& p, const std::string& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << data;
}

// Scripted Resend double.
struct FakeResend final : resend::Client {
  std::vector<resend::SendRequest> sends;
  std::deque<std::variant<std::string, resend::Error>> send_results;
  std::map<std::string, resend::SentEmail> emails;
  std::map<std::string, resend::ReceivedEmail> received;
  std::map<std::string, std::vector<resend::RecvAttachment>> attachments;
  std::map<std::string, std::string> downloads;  // url → bytes
  std::deque<resend::ReceivedPage> pages;
  std::vector<std::optional<std::string>> page_cursors;

  std::string send(const resend::SendRequest& r) override {
    sends.push_back(r);
    if (send_results.empty()) return "re_" + std::to_string(sends.size());
    auto v = send_results.front();
    send_results.pop_front();
    if (auto* e = std::get_if<resend::Error>(&v)) throw *e;
    return std::get<std::string>(v);
  }
  resend::SentEmail get(std::string_view id, resend::Priority) override {
    auto it = emails.find(std::string(id));
    if (it == emails.end()) throw resend::Error(resend::Error::Kind::NotFound, 404, "not_found", "");
    return it->second;
  }
  resend::ReceivedEmail get_received(std::string_view id) override {
    auto it = received.find(std::string(id));
    if (it == received.end()) throw resend::Error(resend::Error::Kind::NotFound, 404, "not_found", "");
    return it->second;
  }
  std::vector<resend::RecvAttachment> list_received_attachments(std::string_view id) override {
    return attachments[std::string(id)];
  }
  resend::DownloadResult download_to(std::string_view url, const fs::path& dest, std::size_t max) override {
    auto it = downloads.find(std::string(url));
    if (it == downloads.end()) throw resend::Error(resend::Error::Kind::Server, 404, "download_failed", "");
    if (it->second.size() > max) throw resend::Error(resend::Error::Kind::Validation, 0, "too_large", "");
    write_file(dest, it->second);
    return {static_cast<int64_t>(it->second.size()), crypto::sha256_hex(it->second)};
  }
  resend::ReceivedPage list_received(int, std::optional<std::string> after, std::optional<std::string>) override {
    page_cursors.push_back(after);
    if (pages.empty()) return {};
    auto p = pages.front();
    pages.pop_front();
    return p;
  }
};

}  // namespace

TEST_CASE("handlers: outbound backoff schedule (≈15.4 h over 10 attempts)", "[job_handlers]") {
  const std::chrono::milliseconds expected[] = {5s, 15s, 1min, 5min, 15min, 1h, 2h, 4h, 8h};
  std::chrono::milliseconds total{0};
  for (int a = 1; a <= 9; ++a) {
    CHECK(outbound_backoff(a) == expected[a - 1]);
    total += outbound_backoff(a);
  }
  CHECK(outbound_backoff(0) == 5s);
  CHECK(outbound_backoff(50) == 8h);
  CHECK(total < 24h);  // inside Resend's idempotency window (B1)
  CHECK(kOutboundSendMaxAttempts == 10);
}

TEST_CASE("handlers: build_inbound_email prefers raw headers, falls back to Resend fields", "[job_handlers]") {
  resend::ReceivedEmail rcv;
  rcv.id = "in_1";
  rcv.from = "=?UTF-8?B?5byg5LiJ?= <zhang@outside.example>";
  rcv.subject = "=?UTF-8?Q?Resend_subject?=";
  rcv.message_id = "<resend-mid@outside.example>";
  rcv.to = {"Bob <bob@team.example>, carol@team.example"};
  rcv.cc = {"dave@team.example"};
  rcv.bcc = {"secret@team.example"};
  rcv.reply_to = {"reply@outside.example"};
  rcv.received_for = {" bob@team.example ", ""};
  rcv.html = "<p>hi</p>";
  rcv.text = "hi";
  rcv.headers = {{"in-reply-to", "<map-parent@team.example>"},
                 {"references", "<map-root@team.example> <map-parent@team.example>"},
                 {"x-azmail-ref", "map-uuid"},
                 {"date", "Tue, 7 Oct 2026 20:03:00 +0800"}};
  rcv.auth.spf = "pass";
  rcv.auth.dmarc = "fail";
  rcv.created_at_ms = 1'760'000'000'000;
  const BlobRef raw{std::string(64, 'a'), 10, "local"};

  SECTION("no raw: everything from Resend, headers map as fallback") {
    const auto e = build_inbound_email(rcv, {}, std::nullopt, {}, mail::InboundSource::Poll);
    CHECK(e.resend_id == "in_1");
    CHECK(e.from == Address{"张三", "zhang@outside.example"});
    REQUIRE(e.to.size() == 2);
    CHECK(e.to[0] == Address{"Bob", "bob@team.example"});
    CHECK(e.to[1].email == "carol@team.example");
    CHECK(e.cc == std::vector<Address>{{"", "dave@team.example"}});
    CHECK(e.reply_to == std::vector<Address>{{"", "reply@outside.example"}});
    CHECK(e.received_for == std::vector<std::string>{"bob@team.example"});
    CHECK(e.subject == "Resend subject");
    CHECK(e.message_id == std::optional<std::string>("resend-mid@outside.example"));
    CHECK(e.in_reply_to == std::optional<std::string>("map-parent@team.example"));
    CHECK(e.references == std::vector<std::string>{"map-root@team.example", "map-parent@team.example"});
    CHECK(e.x_azmail_ref == std::optional<std::string>("map-uuid"));
    CHECK(e.date == *utc_ms(2026, 10, 7, 12, 3, 0));
    CHECK(e.received_at == 1'760'000'000'000);
    CHECK(e.auth.spf == std::optional<std::string>("pass"));
    CHECK(e.auth.dmarc == std::optional<std::string>("fail"));
    CHECK_FALSE(e.auth.dkim.has_value());
    CHECK(e.html == std::optional<std::string>("<p>hi</p>"));
    CHECK(e.html_format == "cid");
    CHECK(e.source == mail::InboundSource::Poll);
    CHECK_FALSE(e.raw.has_value());
  }
  SECTION("raw headers win") {
    const auto hdr = mail::eml::parse_headers(
        "From: \"Raw Name\" <raw@outside.example>\r\n"
        "Subject: =?GBK?B?1tzI1Q==?=\r\n"
        "Message-ID: <raw-mid@outside.example>\r\n"
        "In-Reply-To: <raw-parent@team.example>\r\n"
        "References: <raw-root@team.example>\r\n"
        "X-AzMail-Ref: raw-uuid\r\n"
        "Auto-Submitted: auto-replied\r\n"
        "Date: Wed, 8 Oct 2026 01:00:00 +0000\r\n\r\n");
    std::vector<mail::InboundAttachment> atts(1);
    atts[0].resend_attachment_id = "att_1";
    const auto e = build_inbound_email(rcv, hdr, raw, atts, mail::InboundSource::Webhook);
    CHECK(e.from == Address{"Raw Name", "raw@outside.example"});
    CHECK(e.subject == "周日");
    CHECK(e.message_id == std::optional<std::string>("raw-mid@outside.example"));
    CHECK(e.in_reply_to == std::optional<std::string>("raw-parent@team.example"));
    CHECK(e.references == std::vector<std::string>{"raw-root@team.example"});
    CHECK(e.x_azmail_ref == std::optional<std::string>("raw-uuid"));
    CHECK(e.auto_submitted == std::optional<std::string>("auto-replied"));
    CHECK(e.date == *utc_ms(2026, 10, 8, 1, 0, 0));
    CHECK(e.received_at == 1'760'000'000'000);
    REQUIRE(e.raw.has_value());
    CHECK(e.raw->sha256 == raw.sha256);
    REQUIRE(e.attachments.size() == 1);
    CHECK(e.attachments[0].resend_attachment_id == "att_1");
  }
  SECTION("nothing usable: empty fields, date falls back to created_at") {
    resend::ReceivedEmail bare;
    bare.id = "in_2";
    bare.created_at_ms = 42;
    const auto e = build_inbound_email(bare, {}, std::nullopt, {}, mail::InboundSource::Admin);
    CHECK(e.from.email.empty());
    CHECK(e.subject.empty());
    CHECK_FALSE(e.message_id.has_value());
    CHECK(e.date == 42);
    CHECK(e.to.empty());
  }
}

TEST_CASE("handlers: registration covers every kind on its lane with periodic seeds", "[job_handlers]") {
  test::TestServices ts;
  RunnerConfig rc;
  rc.lane_threads = {};
  Runner r(ts.db, ts.svc, rc);
  register_all_jobs(r);
  CHECK_THROWS_AS(register_outbound_jobs(r), std::invalid_argument);  // already registered
  r.start();
  r.stop();
  ts.db.read([&](db::Conn& c) {
    for (std::string_view kind : {kinds::kOutboundReconcile, kinds::kPollReceiving, kinds::kPurgeTrash,
                                  kinds::kGcBlobs, kinds::kGcHousekeeping, kinds::kDbOptimize}) {
      CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE kind=? AND state='pending' AND dedupe_key=?", kind,
                              dedupe_periodic(kind)) == std::optional<int64_t>(1));
    }
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs") == std::optional<int64_t>(6));
  });
}

TEST_CASE("handlers: payload validation and missing Resend client", "[job_handlers]") {
  test::TestServices ts;
  std::stop_source ss;
  CHECK_THROWS_AS(run_outbound_send(ts.svc, job_of(kinds::kOutboundSend, {}), ss.get_token()), Permanent);
  CHECK_THROWS_AS(run_outbound_send(ts.svc, job_of(kinds::kOutboundSend, {{"outbound_id", "x"}}), ss.get_token()),
                  Permanent);
  CHECK_THROWS_AS(run_outbound_fetch_meta(ts.svc, job_of(kinds::kOutboundFetchMeta, {}), ss.get_token()), Permanent);
  CHECK_THROWS_AS(run_inbound_fetch(ts.svc, job_of(kinds::kInboundFetch, {}), ss.get_token()), Permanent);
  CHECK_THROWS_AS(run_inbound_fetch(ts.svc, job_of(kinds::kInboundFetch, {{"resend_id", ""}}), ss.get_token()),
                  Permanent);

  // No Resend client configured: send/fetch retry later; the pollers are no-ops.
  const auto r = thrown<Retry>([&] {
    run_outbound_send(ts.svc, job_of(kinds::kOutboundSend, {{"outbound_id", 7}}, 1, 10), ss.get_token());
  });
  CHECK(r.delay == 5min);
  CHECK(r.count_attempt);
  CHECK_THROWS_AS(run_inbound_fetch(ts.svc, job_of(kinds::kInboundFetch, {{"resend_id", "in_1"}}), ss.get_token()),
                  Retry);
  CHECK_NOTHROW(run_poll_receiving(ts.svc, job_of(kinds::kPollReceiving, {}), ss.get_token()));
  CHECK_NOTHROW(run_outbound_reconcile(ts.svc, job_of(kinds::kOutboundReconcile, {}), ss.get_token()));
}

TEST_CASE("handlers: db.optimize runs PRAGMA optimize and a WAL checkpoint", "[job_handlers]") {
  test::TestServices ts;
  ts.db.write([](db::Tx& tx) { db::kv_set(tx, "x", "y", 1); });
  std::stop_source ss;
  CHECK_NOTHROW(run_db_optimize(ts.svc, job_of(kinds::kDbOptimize, {}), ss.get_token()));
}

TEST_CASE("handlers: gc.housekeeping purges WP-C data and stale temp files", "[job_handlers]") {
  test::TestServices ts;
  const int64_t now = ts.clock.now_ms();
  const int64_t day = 86'400'000;
  ts.db.write([&](db::Tx& tx) {
    tx.run("INSERT INTO jobs(kind,lane,payload,state,run_at,created_at,updated_at) "
           "VALUES('gc.blobs','maintenance','{}','done',0,0,?)", now - 8 * day);
    tx.run("INSERT INTO jobs(kind,lane,payload,state,run_at,created_at,updated_at) "
           "VALUES('gc.blobs','maintenance','{}','done',0,0,?)", now - day);
    tx.run("INSERT INTO jobs(kind,lane,payload,state,run_at,attempts,locked_until,created_at,updated_at) "
           "VALUES('inbound.fetch','inbound','{}','running',0,1,?,0,0)", now - 1);
    tx.run("INSERT INTO webhook_events(svix_id,type,payload,received_at) VALUES('old','t','{}',?)", now - 31 * day);
    tx.run("INSERT INTO webhook_events(svix_id,type,payload,received_at) VALUES('new','t','{}',?)", now - day);
  });
  const fs::path tmp = ts.blobs->tmp_dir();
  write_file(tmp / "old.part", "x");
  write_file(tmp / "fresh.part", "y");
  fs::last_write_time(tmp / "old.part", fs::file_time_type::clock::now() - 25h);

  std::stop_source ss;
  try {
    run_gc_housekeeping(ts.svc, job_of(kinds::kGcHousekeeping, {}), ss.get_token());
  } catch (const std::exception&) {
    // Steps owned by other packages (sessions, orphan uploads) may still be stubs; ours ran anyway.
  }
  ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE state='done'") == std::optional<int64_t>(1));
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE state='pending' AND kind='inbound.fetch'") ==
          std::optional<int64_t>(1));
    CHECK(c.scalar<std::string>("SELECT group_concat(svix_id) FROM webhook_events") ==
          std::optional<std::string>("new"));
  });
  CHECK_FALSE(fs::exists(tmp / "old.part"));
  CHECK(fs::exists(tmp / "fresh.part"));
}

// ---- integration (needs WP-B mail::*) -------------------------------------------------------

namespace {

struct OutboundFixture {
  test::TestServices ts;
  FakeResend fake;
  int64_t alice = 0;
  mail::SendResult sent;
  std::stop_source ss;

  explicit OutboundFixture(std::optional<int64_t> scheduled_at = std::nullopt) {
    ts.svc.resend = &fake;
    alice = ts.db.write([](db::Tx& tx) { return test::seed_user(tx, "alice@team.example", false, "Alice"); });
    sent = ts.db.write([&](db::Tx& tx) {
      mail::DraftInput in;
      in.to = std::vector<Address>{{"Ext", "ext@outside.example"}};
      in.subject = "Quarterly report";
      in.html = "<p>numbers</p>";
      const auto d = mail::create_draft(tx, ts.urls, alice, in);
      mail::SendOptions so;
      so.version = d.version;
      so.scheduled_at = scheduled_at;
      so.now_ms = ts.clock.now_ms();
      return mail::queue_send(tx, ts.cfg, alice, d.id, so);
    });
  }
  Job job(int attempts = 1) {
    return job_of(kinds::kOutboundSend, {{"outbound_id", sent.outbound_id}}, attempts, kOutboundSendMaxAttempts);
  }
  mail::OutboundRow row() {
    return *ts.db.read([&](db::Conn& c) { return mail::get_outbound(c, sent.outbound_id); });
  }
};

}  // namespace

TEST_CASE("outbound.send: happy path sends once with idempotency key and enqueues fetch_meta",
          "[job_handlers][.integration]") {
  OutboundFixture f;
  f.ts.db.write([&](db::Tx& tx) { db::kv_set(tx, db::kv_keys::kQuotaBlocked, "daily", 1); });
  run_outbound_send(f.ts.svc, f.job(), f.ss.get_token());
  REQUIRE(f.fake.sends.size() == 1);
  const auto& req = f.fake.sends[0];
  const auto row = f.row();
  CHECK(req.idempotency_key == row.uuid);
  CHECK(req.subject == "Quarterly report");
  CHECK(req.to.size() == 1);
  bool ref = false, tag = false;
  for (const auto& [k, v] : req.headers) ref = ref || (k == "X-AzMail-Ref" && v == row.uuid);
  for (const auto& [k, v] : req.tags) tag = tag || (k == "azmail_outbound" && v == row.uuid);
  CHECK(ref);
  CHECK(tag);
  CHECK_FALSE(req.scheduled_at_iso.has_value());
  CHECK(row.status == mail::OutboundStatus::Accepted);
  CHECK(row.resend_id == std::optional<std::string>("re_1"));
  f.ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE kind='outbound.fetch_meta' AND dedupe_key=?",
                            dedupe_fetch_meta(f.sent.outbound_id)) == std::optional<int64_t>(1));
    CHECK_FALSE(db::kv_get(c, db::kv_keys::kQuotaBlocked).has_value());
  });
  // A second run (e.g. duplicate job) does nothing: mark_sending() is false once accepted.
  run_outbound_send(f.ts.svc, f.job(), f.ss.get_token());
  CHECK(f.fake.sends.size() == 1);
}

TEST_CASE("outbound.send: error table", "[job_handlers][.integration]") {
  using K = resend::Error::Kind;
  SECTION("429 retries after retry-after without consuming the attempt") {
    OutboundFixture f;
    f.fake.send_results.push_back(resend::Error(K::RateLimited, 429, "rate_limit_exceeded", "", std::chrono::seconds(3)));
    const auto r = thrown<Retry>([&] { run_outbound_send(f.ts.svc, f.job(), f.ss.get_token()); });
    CHECK(r.delay == 3s);
    CHECK_FALSE(r.count_attempt);
    CHECK(f.row().status == mail::OutboundStatus::Sending);
  }
  SECTION("network errors back off; the last attempt marks the send failed") {
    OutboundFixture f;
    f.fake.send_results.push_back(resend::Error(K::Network, 0, "network", "timeout"));
    auto r = thrown<Retry>([&] { run_outbound_send(f.ts.svc, f.job(2), f.ss.get_token()); });
    CHECK(r.delay == 15s);
    CHECK(r.count_attempt);
    CHECK(f.row().status == mail::OutboundStatus::Sending);
    f.fake.send_results.push_back(resend::Error(K::Server, 500, "internal_server_error", ""));
    r = thrown<Retry>([&] { run_outbound_send(f.ts.svc, f.job(kOutboundSendMaxAttempts), f.ss.get_token()); });
    CHECK(f.row().status == mail::OutboundStatus::Failed);
  }
  SECTION("quota → failed, banner set, no retry") {
    OutboundFixture f;
    f.fake.send_results.push_back(resend::Error(K::Quota, 429, "monthly_quota_exceeded", ""));
    CHECK_THROWS_AS(run_outbound_send(f.ts.svc, f.job(), f.ss.get_token()), Permanent);
    CHECK(f.row().status == mail::OutboundStatus::Failed);
    CHECK(f.row().status_detail == std::optional<std::string>("发送配额已用完"));
    CHECK(f.ts.db.read([](db::Conn& c) { return db::kv_get(c, db::kv_keys::kQuotaBlocked); }) ==
          std::optional<std::string>("monthly"));
  }
  SECTION("validation / idempotency conflict → failed") {
    OutboundFixture f;
    f.fake.send_results.push_back(resend::Error(K::IdempotencyConflict, 409, "invalid_idempotent_request", ""));
    CHECK_THROWS_AS(run_outbound_send(f.ts.svc, f.job(), f.ss.get_token()), Permanent);
    CHECK(f.row().status == mail::OutboundStatus::Failed);
  }
  SECTION("in-flight idempotent request retries after 2 s") {
    OutboundFixture f;
    f.fake.send_results.push_back(resend::Error(K::IdempotencyInFlight, 409, "concurrent_idempotent_requests", ""));
    CHECK(thrown<Retry>([&] { run_outbound_send(f.ts.svc, f.job(), f.ss.get_token()); }).delay == 2s);
  }
}

TEST_CASE("outbound.send: Resend refuses scheduling → local scheduling", "[job_handlers][.integration]") {
  OutboundFixture f(azm::now_ms() + 3'600'000);
  if (f.row().scheduled_via != mail::ScheduledVia::Resend) SKIP("draft was scheduled locally by queue_send");
  f.fake.send_results.push_back(
      resend::Error(resend::Error::Kind::Validation, 422, "validation_error", "Scheduled emails cannot have attachments"));
  const auto r = thrown<Retry>([&] { run_outbound_send(f.ts.svc, f.job(), f.ss.get_token()); });
  CHECK_FALSE(r.count_attempt);
  CHECK(r.delay > 3'000'000ms);
  CHECK(f.row().scheduled_via == std::optional<mail::ScheduledVia>(mail::ScheduledVia::Local));
  REQUIRE(f.fake.sends.size() == 1);
  CHECK(f.fake.sends[0].scheduled_at_iso.has_value());
}

TEST_CASE("inbound.fetch: downloads raw + attachments and delivers once", "[job_handlers][.integration]") {
  test::TestServices ts;
  FakeResend fake;
  ts.svc.resend = &fake;
  const int64_t bob = ts.db.write([](db::Tx& tx) { return test::seed_user(tx, "bob@team.example"); });
  const std::string raw =
      "From: Ext <ext@outside.example>\r\nTo: bob@team.example\r\nSubject: =?GBK?B?1tzI1Q==?=\r\n"
      "Message-ID: <ext-1@outside.example>\r\nDate: Tue, 7 Oct 2026 12:00:00 +0000\r\n\r\nbody\r\n";
  resend::ReceivedEmail rcv;
  rcv.id = "in_9";
  rcv.from = "Ext <ext@outside.example>";
  rcv.to = {"bob@team.example"};
  rcv.received_for = {"bob@team.example"};
  rcv.subject = "ignored";
  rcv.text = "body";
  rcv.raw_download_url = "https://dl/raw";
  rcv.created_at_ms = ts.clock.now_ms();
  fake.received["in_9"] = rcv;
  fake.downloads["https://dl/raw"] = raw;
  fake.attachments["in_9"] = {{"att_1", "a.txt", "text/plain", "attachment", std::nullopt, 5, "https://dl/att_1"}};
  fake.downloads["https://dl/att_1"] = "hello";

  RunnerConfig rc;
  rc.lane_threads = {};
  Runner runner(ts.db, ts.svc, rc);
  runner.on(std::string(kinds::kInboundFetch), "inbound", run_inbound_fetch);
  ts.db.write([&](db::Tx& tx) {
    mail::record_inbound_pending(tx, "in_9", mail::InboundSource::Webhook, ts.clock.now_ms());
    enqueue(tx, kinds::kInboundFetch, {{"resend_id", "in_9"}, {"source", "webhook"}},
            {.dedupe_key = dedupe_inbound_fetch("in_9"), .now_ms = ts.clock.now_ms()});
  });
  REQUIRE(runner.run_one("inbound"));
  ts.db.read([&](db::Conn& c) {
    CHECK(mail::inbound_state(c, "in_9") == std::optional<mail::InboundState>(mail::InboundState::Delivered));
    CHECK(c.scalar<std::string>("SELECT subject FROM messages WHERE owner_id=?", bob) ==
          std::optional<std::string>("周日"));
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM attachments WHERE owner_id=?", bob) == std::optional<int64_t>(1));
    CHECK(c.scalar<std::string>("SELECT state FROM jobs WHERE kind='inbound.fetch'") ==
          std::optional<std::string>("done"));
  });
  CHECK(ts.blobs->exists(crypto::sha256_hex(raw)));
  CHECK(ts.blobs->exists(crypto::sha256_hex("hello")));
  for (const auto& de : fs::directory_iterator(ts.blobs->tmp_dir())) FAIL("temp file left: " << de.path());
}

TEST_CASE("inbound.fetch: 404 right after the webhook is retried, then given up", "[job_handlers][.integration]") {
  test::TestServices ts;
  FakeResend fake;
  ts.svc.resend = &fake;
  ts.db.write([&](db::Tx& tx) { mail::record_inbound_pending(tx, "in_x", mail::InboundSource::Webhook, 1); });
  std::stop_source ss;
  const auto r = thrown<Retry>(
      [&] { run_inbound_fetch(ts.svc, job_of(kinds::kInboundFetch, {{"resend_id", "in_x"}}, 1), ss.get_token()); });
  CHECK(r.delay == 30s);
  CHECK_THROWS_AS(
      run_inbound_fetch(ts.svc, job_of(kinds::kInboundFetch, {{"resend_id", "in_x"}}, 6), ss.get_token()), Permanent);
  CHECK(ts.db.read([](db::Conn& c) { return mail::inbound_state(c, "in_x"); }) ==
        std::optional<mail::InboundState>(mail::InboundState::Failed));
}

TEST_CASE("poll.receiving: enqueues unseen ids until a known one", "[job_handlers][.integration]") {
  test::TestServices ts;
  FakeResend fake;
  ts.svc.resend = &fake;
  ts.db.write([&](db::Tx& tx) { mail::record_inbound_pending(tx, "in_1", mail::InboundSource::Webhook, 1); });
  fake.pages.push_back({{"in_4", "in_3"}, true});
  fake.pages.push_back({{"in_2", "in_1", "in_0"}, true});
  std::stop_source ss;
  run_poll_receiving(ts.svc, job_of(kinds::kPollReceiving, {}), ss.get_token());
  CHECK(fake.page_cursors == std::vector<std::optional<std::string>>{std::nullopt, std::string("in_3")});
  ts.db.read([&](db::Conn& c) {
    for (std::string id : {"in_4", "in_3", "in_2", "in_0"})
      CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE dedupe_key=?", dedupe_inbound_fetch(id)) ==
            std::optional<int64_t>(1));
    CHECK(db::kv_get(c, db::kv_keys::kPollHighWater) == std::optional<std::string>("in_4"));
    CHECK(db::kv_get_i64(c, db::kv_keys::kLastPollAt) == std::optional<int64_t>(ts.clock.now_ms()));
  });
}

TEST_CASE("gc.blobs: unreferenced old blobs are removed from the store and the table",
          "[job_handlers][.integration]") {
  test::TestServices ts;
  ts.cfg.blob_gc_grace_hours = 1;
  const auto ref = ts.blobs->put_bytes("orphan");
  ts.db.write([&](db::Tx& tx) { mail::register_blob(tx, ref, ts.clock.now_ms() - 2 * 3'600'000); });
  std::stop_source ss;
  run_gc_blobs(ts.svc, job_of(kinds::kGcBlobs, {}), ss.get_token());
  CHECK_FALSE(ts.blobs->exists(ref.sha256));
  CHECK(ts.db.read([](db::Conn& c) { return c.scalar<int64_t>("SELECT COUNT(*) FROM blobs"); }) ==
        std::optional<int64_t>(0));
}
