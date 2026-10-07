// Owner: WP-B — outbound status precedence (B3): the full (current, event) table, out-of-order and
// duplicate events, informational events, canceled as terminal, bounce details, correlation by
// resend id / uuid (B4), Message-ID capture from events, WS fan-out per status change.
#include "send_fixtures.hpp"

#include "jobs/kinds.hpp"
#include "ws/events.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::sendfx;

namespace {

// The precedence of DESIGN B3, written out independently of types.hpp.
const std::map<std::string, int> kRank = {
    {"queued", 0},   {"sending", 1},          {"accepted", 2},  {"scheduled", 2},
    {"sent", 3},     {"delivery_delayed", 4}, {"delivered", 5}, {"complained", 6},
    {"bounced", 7},  {"failed", 7},           {"suppressed", 7}, {"canceled", 8}};

// Event type that implies each status (statuses without one only arise locally).
const std::map<std::string, std::string> kEventFor = {
    {"scheduled", "email.scheduled"},   {"sent", "email.sent"},
    {"delivery_delayed", "email.delivery_delayed"}, {"delivered", "email.delivered"},
    {"complained", "email.complained"}, {"bounced", "email.bounced"},
    {"failed", "email.failed"},         {"suppressed", "email.suppressed"},
    {"canceled", "local.canceled"}};

struct Sent {
  SendResult r;
  std::string resend_id;
};

Sent accepted_send(SendFx& fx, const std::string& resend_id) {
  Sent s{fx.send(fx.simple_draft({kExternal})), resend_id};
  fx.accept(s.r.outbound_id, resend_id);
  return s;
}

void force_status(SendFx& fx, int64_t outbound_id, const std::string& st) {
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE outbound SET status = ? WHERE id = ?", st, outbound_id); });
}

}  // namespace

TEST_CASE("status precedence: every (current, event) pair", "[status][precedence]") {
  SendFx fx;
  const Sent s = accepted_send(fx, "re_table");
  int n = 0;
  for (const auto& [cur, cur_rank] : kRank) {
    for (const auto& [next, type] : kEventFor) {
      force_status(fx, s.r.outbound_id, cur);
      const EventApplyResult res = fx.event(s.resend_id, type, "svix-" + std::to_string(++n));
      const bool expect_apply = cur != "canceled" && kRank.at(next) > cur_rank;
      INFO(cur << " + " << type);
      CHECK(res.outcome == EventOutcome::Applied);
      CHECK(res.outbound_id == s.r.outbound_id);
      CHECK(res.status_changed == expect_apply);
      const std::string after(to_string(fx.outbound(s.r.outbound_id).status));
      CHECK(after == (expect_apply ? next : cur));
      REQUIRE(res.status);
      CHECK(to_string(*res.status) == after);
      // types.hpp agrees with the written-out table.
      CHECK(should_apply(*parse_outbound_status(cur), *parse_outbound_status(next)) == expect_apply);
    }
  }
  // Every event was logged, applied or not.
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND source_key LIKE 'svix-%'",
                     s.r.outbound_id) == n);
}

TEST_CASE("status precedence: shuffled real-world sequences", "[status][precedence]") {
  SendFx fx;
  const int64_t t0 = azm::now_ms() + 60'000;  // after the local queue/send/accept events
  SECTION("delay then delivered, delivered first") {
    const Sent s = accepted_send(fx, "re_shuffle");
    fx.event(s.resend_id, "email.delivered", "e3", t0 + 3000);
    fx.event(s.resend_id, "email.sent", "e1", t0 + 1000);
    fx.event(s.resend_id, "email.delivery_delayed", "e2", t0 + 2000, {{"message", "mailbox busy"}});
    const auto row = fx.outbound(s.r.outbound_id);
    CHECK(row.status == OutboundStatus::Delivered);
    CHECK(row.last_event == "delivered");  // older events don't regress last_event
    CHECK(row.last_event_at == t0 + 3000);
    CHECK_FALSE(row.status_detail);
    const auto events =
        fx.ts.db.read([&](db::Conn& c) { return message_events(c, fx.alice, s.r.message_id); }).value();
    // local.queued, local.sending, local.accepted, then the three webhooks by occurred_at
    REQUIRE(events.size() == 6);
    CHECK(events[3].type == "email.sent");
    CHECK(events[4].type == "email.delivery_delayed");
    CHECK(events[5].type == "email.delivered");
  }
  SECTION("complained after delivered; bounced is final with its text") {
    const Sent s = accepted_send(fx, "re_complain");
    fx.event(s.resend_id, "email.delivered", "a1", t0);
    fx.event(s.resend_id, "email.complained", "a2", t0 + 1);
    CHECK(fx.outbound(s.r.outbound_id).status == OutboundStatus::Complained);
    const Sent b = accepted_send(fx, "re_bounce");
    fx.event(b.resend_id, "email.bounced", "b1", t0,
             {{"bounce", {{"message", "550 5.1.1 用户不存在"}, {"type", "Permanent"}}}});
    fx.event(b.resend_id, "email.delivered", "b2", t0 + 5);  // late, lower rank
    const auto row = fx.outbound(b.r.outbound_id);
    CHECK(row.status == OutboundStatus::Bounced);
    CHECK(row.status_detail == "550 5.1.1 用户不存在");
    CHECK(fx.message(b.r.message_id)->outbound->status_detail == "550 5.1.1 用户不存在");
    fx.event(b.resend_id, "email.suppressed", "b3", t0 + 6);  // equal rank never replaces
    CHECK(fx.outbound(b.r.outbound_id).status == OutboundStatus::Bounced);
  }
  SECTION("failed reason") {
    const Sent s = accepted_send(fx, "re_fail");
    fx.event(s.resend_id, "email.failed", "f1", t0, {{"failed", {{"reason", "reached_daily_quota"}}}});
    CHECK(fx.outbound(s.r.outbound_id).status_detail == "reached_daily_quota");
  }
}

TEST_CASE("duplicate, informational and unknown events", "[status][events]") {
  SendFx fx;
  const Sent s = accepted_send(fx, "re_dup");
  CHECK(fx.event(s.resend_id, "email.delivered", "svix-1").outcome == EventOutcome::Applied);
  const auto dup = fx.event(s.resend_id, "email.delivered", "svix-1");
  CHECK(dup.outcome == EventOutcome::Duplicate);
  CHECK(dup.outbound_id == s.r.outbound_id);
  CHECK_FALSE(dup.status_changed);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE source_key = 'svix-1'") == 1);
  // The same svix id may exist for another outbound (UNIQUE per outbound).
  const Sent other = accepted_send(fx, "re_other");
  CHECK(fx.event(other.resend_id, "email.sent", "svix-1").outcome == EventOutcome::Applied);

  fx.ts.notifier.clear();
  const auto opened = fx.event(s.resend_id, "email.opened", "svix-2", azm::now_ms() + 1000);
  CHECK(opened.outcome == EventOutcome::Applied);
  CHECK_FALSE(opened.status_changed);
  CHECK(fx.event(s.resend_id, "email.clicked", "svix-3", azm::now_ms() + 2000).outcome == EventOutcome::Applied);
  CHECK(fx.outbound(s.r.outbound_id).status == OutboundStatus::Delivered);
  CHECK(fx.outbound(s.r.outbound_id).last_event == "clicked");
  CHECK(fx.ts.notifier.events_of(std::string(ws::events::kOutboundStatus)).empty());  // no status change → no hint

  const auto unknown = fx.event(std::string("re_someone_else"), "email.delivered", "svix-9");
  CHECK(unknown.outcome == EventOutcome::Unknown);
  CHECK_FALSE(unknown.outbound_id);
  CHECK(fx.scalar("SELECT COUNT(*) FROM delivery_events WHERE source_key = 'svix-9'") == 0);
  CHECK(fx.event(std::nullopt, "email.sent", "svix-10", 0, {}, std::string("not-a-uuid")).outcome ==
        EventOutcome::Unknown);
}

TEST_CASE("canceled is local-only and terminal", "[status][events]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE user_settings SET undo_send_seconds = 30"); });
  const SendResult r = fx.send(fx.simple_draft({kExternal}));
  const std::string uuid = fx.outbound(r.outbound_id).uuid;
  fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.alice, r.message_id); });
  const auto res = fx.event(std::nullopt, "email.sent", "late", 0, {}, uuid);
  CHECK(res.outcome == EventOutcome::Applied);
  CHECK_FALSE(res.status_changed);
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
  fx.ts.db.write([&](db::Tx& tx) { mark_failed(tx, r.outbound_id, "x", "发送失败"); });
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
  fx.ts.db.write([&](db::Tx& tx) { mark_accepted(tx, r.outbound_id, "re_late", false); });
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
}

TEST_CASE("local transitions never contradict Resend", "[status][events]") {
  SendFx fx;
  // A webhook said sent; a failing POST retry must not turn it into failed.
  const SendResult r = fx.send(fx.simple_draft({kExternal}));
  fx.ts.db.write([&](db::Tx& tx) { mark_sending(tx, r.outbound_id); });
  fx.event(std::string("re_w"), "email.sent", "w", 0, {}, fx.outbound(r.outbound_id).uuid);
  fx.ts.db.write([&](db::Tx& tx) { mark_failed(tx, r.outbound_id, "internal_server_error", "发送失败"); });
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Sent);
  CHECK_FALSE(fx.outbound(r.outbound_id).error_name);
  // A failed send may be re-marked (detail refreshed).
  const SendResult f = fx.send(fx.simple_draft({kExternal}));
  fx.ts.db.write([&](db::Tx& tx) {
    mark_failed(tx, f.outbound_id, "a", "第一次");
    mark_failed(tx, f.outbound_id, "b", "第二次");
  });
  CHECK(fx.outbound(f.outbound_id).status_detail == "第二次");
  // "scheduled" acceptance of a send without scheduled_at is a plain acceptance.
  const SendResult n = fx.send(fx.simple_draft({kExternal}));
  fx.accept(n.outbound_id, "re_n", /*scheduled=*/true);
  CHECK(fx.outbound(n.outbound_id).status == OutboundStatus::Accepted);
  CHECK(fx.folder_ids(Folder::Sent).size() == 3);
}

TEST_CASE("webhook before commit / lost POST response: correlation by uuid tag (B4)", "[status][events][b4]") {
  SendFx fx;
  const SendResult r = fx.send(fx.simple_draft({kExternal}));
  fx.ts.db.write([&](db::Tx& tx) { mark_sending(tx, r.outbound_id); });
  const std::string uuid = fx.outbound(r.outbound_id).uuid;
  // email.sent arrives with the azmail_outbound tag while resend_id is still unknown locally.
  const auto res = fx.event(std::string("re_lost"), "email.sent", "w1", 0, {}, uuid);
  CHECK(res.outcome == EventOutcome::Applied);
  CHECK(res.status_changed);
  auto row = fx.outbound(r.outbound_id);
  CHECK(row.status == OutboundStatus::Sent);
  CHECK(row.resend_id == "re_lost");
  // fetch_meta is enqueued to learn the Message-ID (dedupe out:meta:<id>, +10 s).
  const std::string key = jobs::dedupe_fetch_meta(r.outbound_id);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM jobs WHERE dedupe_key = ? AND kind = 'outbound.fetch_meta'", key) == 1);
  CHECK(fx.scalar_of("SELECT run_at FROM jobs WHERE dedupe_key = ?", key) >= azm::now_ms() + 5'000);
  // The send job sees that the send went through and stops.
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r.outbound_id); }));
  // Later events correlate by resend_id; the retried POST's acceptance doesn't regress status.
  fx.event(std::string("re_lost"), "email.delivered", "w2", 0, {}, uuid);
  fx.ts.db.write([&](db::Tx& tx) { mark_accepted(tx, r.outbound_id, "re_lost", false); });
  row = fx.outbound(r.outbound_id);
  CHECK(row.status == OutboundStatus::Delivered);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM jobs WHERE dedupe_key = ?", key) == 1);

  // With the Message-ID in the event, no fetch_meta is needed.
  const SendResult r2 = fx.send(fx.simple_draft({kExternal}));
  fx.ts.db.write([&](db::Tx& tx) { mark_sending(tx, r2.outbound_id); });
  fx.event(std::string("re_lost2"), "email.sent", "w3", 0, {}, fx.outbound(r2.outbound_id).uuid,
           std::string("<m2@resend.dev>"));
  CHECK(fx.outbound(r2.outbound_id).message_id_header == "m2@resend.dev");
  CHECK(fx.text_of("SELECT message_id_header FROM messages WHERE id = ?", r2.message_id) == "m2@resend.dev");
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM jobs WHERE dedupe_key = ?", jobs::dedupe_fetch_meta(r2.outbound_id)) == 0);
}

TEST_CASE("status changes fan out to every copy; folder moves emit threads.changed", "[status][ws]") {
  SendFx fx;
  DraftInput in;
  in.to = std::vector<Address>{kCustomer};
  in.from_address_id = fx.support;
  const SendResult r = fx.send(fx.create(in), azm::now_ms() + 3'600'000);
  CHECK(fx.folder_ids(Folder::Scheduled).size() == 1);
  CHECK(fx.folder_ids(Folder::Scheduled, fx.bob).size() == 1);  // bob's shared copy is scheduled too
  fx.accept(r.outbound_id, "re_sch", true);
  fx.ts.notifier.clear();
  // The scheduled time comes: email.sent moves both copies from Scheduled to Sent.
  fx.event(std::string("re_sch"), "email.sent", "x1", 0, {{"to", boost::json::array{"wang@customer.example"}}});
  for (int64_t owner : {fx.alice, fx.bob}) {
    CHECK(fx.events_of(ws::events::kOutboundStatus, owner).size() == 1);
    CHECK(fx.events_of(ws::events::kThreadsChanged, owner).size() == 1);
    CHECK(fx.folder_ids(Folder::Scheduled, owner).empty());
    CHECK(fx.folder_ids(Folder::Sent, owner).size() == 1);
    const auto items = fx.folder(Folder::Sent, owner).items;
    CHECK(items.at(0).latest_status == OutboundStatus::Sent);
  }
  const auto ev = fx.events_of(ws::events::kOutboundStatus, fx.alice).at(0);
  CHECK(ev.data.at("outbound_id").as_int64() == r.outbound_id);
  CHECK(ev.data.at("message_id").as_int64() == r.message_id);
  CHECK(ev.data.at("thread_id").as_int64() == r.thread_id);
  CHECK(ev.data.at("status").as_string() == "sent");
  CHECK(ev.data.at("status_detail").is_null());
  // delivered: same folder → outbound.status only.
  fx.ts.notifier.clear();
  fx.event(std::string("re_sch"), "email.delivered", "x2");
  CHECK(fx.events_of(ws::events::kOutboundStatus, fx.bob).size() == 1);
  CHECK(fx.events_of(ws::events::kThreadsChanged, fx.bob).empty());
}

TEST_CASE("event detail keeps the recipients", "[status][events]") {
  SendFx fx;
  const Sent s = accepted_send(fx, "re_rcpt");
  OutboundEvent ev;
  ev.resend_id = "re_rcpt";
  ev.type = "email.delivered";
  ev.source_key = "r1";
  ev.recipients = {"ext@other.example"};
  fx.ts.db.write([&](db::Tx& tx) { apply_outbound_event(tx, ev); });
  const auto events = fx.ts.db.read([&](db::Conn& c) { return message_events(c, fx.alice, s.r.message_id); }).value();
  const auto& d = events.back().detail;
  REQUIRE(d.contains("to"));
  CHECK(d.at("to").as_array().at(0).as_string() == "ext@other.example");
  // IDOR: bob cannot read alice's delivery events.
  CHECK_FALSE(fx.ts.db.read([&](db::Conn& c) { return message_events(c, fx.bob, s.r.message_id); }));
}
