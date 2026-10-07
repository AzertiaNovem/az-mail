// Owner: WP-C — jobs::process_webhook. Cases that only touch WP-C tables run by default; cases
// that need WP-B's mail::record_inbound_pending / apply_outbound_event are [.integration].
#include "db/kv.hpp"
#include "jobs/kinds.hpp"
#include "jobs/webhook_dispatch.hpp"
#include "mail/drafts.hpp"
#include "mail/outbound.hpp"
#include "mail/inbound.hpp"
#include "resend/types.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace azm;
using namespace azm::jobs;

namespace {

struct EventRow {
  std::string type;
  std::optional<std::string> email_id;
  std::string payload;
  int64_t received_at = 0;
  std::optional<int64_t> processed_at;
  std::optional<std::string> result;
};

std::optional<EventRow> event_row(test::TestServices& ts, std::string_view svix_id) {
  return ts.db.read([&](db::Conn& c) -> std::optional<EventRow> {
    auto s = c.prepare(
        "SELECT type, resend_email_id, payload, received_at, processed_at, result FROM webhook_events WHERE svix_id=?");
    s.bind_all(svix_id);
    if (!s.step()) return std::nullopt;
    return EventRow{s.text(0), s.opt_text(1), s.text(2), s.i64(3), s.opt_i64(4), s.opt_text(5)};
  });
}

WebhookOutcome dispatch(test::TestServices& ts, std::string_view svix_id, const std::string& body, int64_t now) {
  const auto env = resend::parse_webhook(body);
  return ts.db.write([&](db::Tx& tx) { return process_webhook(tx, env, svix_id, body, now); });
}

}  // namespace

TEST_CASE("webhook: result spellings", "[webhook]") {
  CHECK(to_string(WebhookResult::Applied) == "applied");
  CHECK(to_string(WebhookResult::Enqueued) == "enqueued");
  CHECK(to_string(WebhookResult::IgnoredUnknown) == "ignored_unknown");
  CHECK(to_string(WebhookResult::Duplicate) == "duplicate");
  CHECK(to_string(WebhookResult::Error) == "error");
}

TEST_CASE("webhook: unknown types are recorded and ignored; svix_id replays are duplicates", "[webhook]") {
  test::TestServices ts;
  const std::string body = R"({"type":"domain.updated","created_at":"2026-10-07T00:00:00Z","data":{"id":"d1"}})";
  const auto first = dispatch(ts, "msg_1", body, 1'700'000'000'000);
  CHECK(first.result == WebhookResult::IgnoredUnknown);
  auto row = event_row(ts, "msg_1");
  REQUIRE(row.has_value());
  CHECK(row->type == "domain.updated");
  CHECK_FALSE(row->email_id.has_value());
  CHECK(row->payload == body);
  CHECK(row->received_at == 1'700'000'000'000);
  CHECK(row->processed_at == std::optional<int64_t>(1'700'000'000'000));
  CHECK(row->result == std::optional<std::string>("ignored_unknown"));
  CHECK(ts.db.read([](db::Conn& c) { return db::kv_get_i64(c, db::kv_keys::kLastWebhookAt); }) ==
        std::optional<int64_t>(1'700'000'000'000));

  const auto again = dispatch(ts, "msg_1", body, 1'700'000'009'999);
  CHECK(again.result == WebhookResult::Duplicate);
  row = event_row(ts, "msg_1");
  CHECK(row->processed_at == std::optional<int64_t>(1'700'000'000'000));  // untouched
  CHECK(ts.db.read([](db::Conn& c) { return db::kv_get_i64(c, db::kv_keys::kLastWebhookAt); }) ==
        std::optional<int64_t>(1'700'000'000'000));
}

TEST_CASE("webhook: malformed data is recorded as error, not thrown", "[webhook]") {
  test::TestServices ts;
  auto out = dispatch(ts, "msg_r", R"({"type":"email.received","data":{"to":["a@team.example"]}})", 5);
  CHECK(out.result == WebhookResult::Error);
  CHECK(out.detail == "missing_email_id");
  CHECK(event_row(ts, "msg_r")->result == std::optional<std::string>("error:missing_email_id"));
  out = dispatch(ts, "msg_o", R"({"type":"email.delivered","data":{"to":["x@y.example"],"tags":{"other":"1"}}})", 6);
  CHECK(out.result == WebhookResult::Error);
  CHECK(event_row(ts, "msg_o")->result == std::optional<std::string>("error:missing_email_id"));
  // Nothing was enqueued.
  CHECK(ts.db.read([](db::Conn& c) { return c.scalar<int64_t>("SELECT COUNT(*) FROM jobs"); }) ==
        std::optional<int64_t>(0));
}

// ---- integration (needs WP-B mail::*) -------------------------------------------------------

TEST_CASE("webhook: email.received records the inbound and enqueues one fetch", "[webhook][.integration]") {
  test::TestServices ts;
  const int64_t now = ts.clock.now_ms();
  const std::string body = R"({"type":"email.received","created_at":"2026-10-07T00:00:00Z",
                               "data":{"email_id":"in_42","to":["bob@team.example"]}})";
  REQUIRE(dispatch(ts, "msg_a", body, now).result == WebhookResult::Enqueued);
  REQUIRE(dispatch(ts, "msg_b", body, now).result == WebhookResult::Enqueued);  // Svix retry w/ new id
  const auto row = event_row(ts, "msg_a");
  REQUIRE(row.has_value());
  CHECK(row->email_id == std::optional<std::string>("in_42"));
  CHECK(row->result == std::optional<std::string>("enqueued"));
  ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM jobs WHERE kind='inbound.fetch' AND dedupe_key='in:in_42'") ==
          std::optional<int64_t>(1));
    auto s = c.prepare("SELECT payload, priority, run_at FROM jobs WHERE kind='inbound.fetch'");
    REQUIRE(s.step());
    const auto payload = boost::json::parse(s.text(0)).as_object();
    CHECK(payload.at("resend_id").as_string() == "in_42");
    CHECK(payload.at("source").as_string() == "webhook");
    CHECK(s.i64(1) == kPriorityNormal);
    CHECK(s.i64(2) == now);
    CHECK(mail::inbound_state(c, "in_42") == std::optional<mail::InboundState>(mail::InboundState::Pending));
  });
}

TEST_CASE("webhook: delivery events apply to our outbound (tag fallback); others are ignored",
          "[webhook][.integration]") {
  test::TestServices ts;
  const int64_t now = ts.clock.now_ms();
  const int64_t alice = ts.db.write([](db::Tx& tx) { return test::seed_user(tx, "alice@team.example"); });
  const auto sent = ts.db.write([&](db::Tx& tx) {
    mail::DraftInput in;
    in.to = std::vector<Address>{{"Ext", "ext@outside.example"}};
    in.subject = "hello";
    in.html = "<p>hi</p>";
    const auto d = mail::create_draft(tx, ts.urls, alice, in);
    mail::SendOptions so;
    so.version = d.version;
    so.now_ms = now;
    return mail::queue_send(tx, ts.cfg, alice, d.id, so);
  });
  const auto uuid = ts.db.read([&](db::Conn& c) { return mail::get_outbound(c, sent.outbound_id)->uuid; });

  // B4: the webhook arrives before resend_id is committed → matched by the azmail_outbound tag.
  const std::string body = R"({"type":"email.sent","created_at":"2026-10-07T00:00:01Z","data":{
      "email_id":"re_77","to":["ext@outside.example"],"subject":"hello","tags":{"azmail_outbound":")" +
                           uuid + R"("}}})";
  auto out = dispatch(ts, "msg_s", body, now);
  REQUIRE(out.result == WebhookResult::Applied);
  REQUIRE(event_row(ts, "msg_s").has_value());
  CHECK(event_row(ts, "msg_s")->result == std::optional<std::string>("applied"));
  ts.db.read([&](db::Conn& c) {
    CHECK(c.scalar<int64_t>("SELECT COUNT(*) FROM delivery_events WHERE outbound_id=? AND source_key='msg_s'",
                            sent.outbound_id) == std::optional<int64_t>(1));
    const auto detail = c.scalar<std::string>("SELECT detail_json FROM delivery_events WHERE source_key='msg_s'");
    REQUIRE(detail.has_value());
    CHECK(detail->find("hello") == std::string::npos);  // mail content is not copied into details
  });

  // Another app's mail on a shared Resend account (B9).
  out = dispatch(ts, "msg_x", R"({"type":"email.delivered","data":{"email_id":"re_other","to":["z@z.example"]}})", now);
  REQUIRE(out.result == WebhookResult::IgnoredUnknown);
  REQUIRE(event_row(ts, "msg_x").has_value());
  CHECK(event_row(ts, "msg_x")->result == std::optional<std::string>("ignored_unknown"));
}
