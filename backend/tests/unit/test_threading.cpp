// Owner: WP-B — threading (B2, C7), merge, recompute_thread aggregates (§2 table).
#include "mail/threads.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"
#include "ws/events.hpp"

#include <boost/json/parse.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::mailfx;

namespace {

struct Fx {
  test::TestServices ts;
  int64_t alice = 0, bob = 0;
  int64_t alice_addr = 0;
  const Address me{"Alice", "alice@team.example"};
  const Address carol{"Carol", "carol@ext.example"};
  const Address dave{"Dave", "dave@ext.example"};

  Fx() {
    ts.db.write([&](db::Tx& tx) {
      alice = test::seed_user(tx, "alice@team.example", false, "Alice");
      bob = test::seed_user(tx, "bob@team.example", false, "Bob");
      alice_addr = test::address_id(tx.conn(), "alice@team.example");
    });
  }

  Inserted add(Msg m) {
    if (m.owner == 0) m.owner = alice;
    return ts.db.write([&](db::Tx& tx) { return insert_message(tx, m); });
  }
  Msg in_msg(std::string subject, std::optional<std::string> msgid = {}, std::vector<std::string> refs = {},
             int64_t date = kT0) const {
    Msg m;
    m.owner = alice;
    m.from = carol;
    m.to = {me};
    m.subject = std::move(subject);
    m.message_id = std::move(msgid);
    m.refs = std::move(refs);
    m.date = date;
    return m;
  }
  int64_t thread(int64_t msg) {
    return ts.db.read([&](db::Conn& c) { return thread_of(c, msg); });
  }
  Agg agg(int64_t thread_id) {
    return ts.db.read([&](db::Conn& c) { return aggregates(c, thread_id); }).value();
  }
  bool thread_exists(int64_t t) {
    return ts.db.read([&](db::Conn& c) { return c.scalar<int64_t>("SELECT 1 FROM threads WHERE id=?", t).has_value(); });
  }
};

}  // namespace

TEST_CASE("normalize_subject", "[threading]") {
  CHECK(normalize_subject("Hello World") == "hello world");
  CHECK(normalize_subject("Re: Hello") == "hello");
  CHECK(normalize_subject("RE: re: Re:Hello") == "hello");
  CHECK(normalize_subject("Fwd: FW: fw: Hello") == "hello");
  CHECK(normalize_subject("Re[2]: Hello") == "hello");
  CHECK(normalize_subject("Re[3] Hello") == "hello");
  CHECK(normalize_subject("Re(2): Hello") == "hello");
  CHECK(normalize_subject("Re : Hello") == "hello");
  CHECK(normalize_subject("回复：周报") == "周报");
  CHECK(normalize_subject("回复: 答复：转发：周报") == "周报");
  CHECK(normalize_subject("回覆：轉寄：週報") == "週報");
  CHECK(normalize_subject("Re：周报") == "周报");  // full-width colon after an ASCII prefix
  CHECK(normalize_subject("  Re:   lots   of    space  ") == "lots of space");
  CHECK(normalize_subject("Re:　周报") == "周报");
  // List tags are kept; prefixes around them are removed.
  CHECK(normalize_subject("[team] 周报") == "[team] 周报");
  CHECK(normalize_subject("Re: [team] 周报") == "[team] 周报");
  CHECK(normalize_subject("[team] Re: 周报") == "[team] 周报");
  CHECK(normalize_subject("Re: 回复：[ops] 周报") == "[ops] 周报");
  CHECK(normalize_subject("[ops] Re: [ops] x") == "[ops] x");
  CHECK(normalize_subject("[ONLY]") == "[only]");
  // Not prefixes.
  CHECK(normalize_subject("Regarding: plans") == "regarding: plans");
  CHECK(normalize_subject("Re plans") == "re plans");
  CHECK(normalize_subject("Fwiw: ok") == "fwiw: ok");
  CHECK(normalize_subject("回复函：通知") == "回复函：通知");
  CHECK(normalize_subject("") == "");
  CHECK(normalize_subject("Re:") == "");
}

TEST_CASE("has_reply_prefix", "[threading]") {
  CHECK(has_reply_prefix("Re: x"));
  CHECK(has_reply_prefix("re:x"));
  CHECK(has_reply_prefix("FW: x"));
  CHECK(has_reply_prefix("Fwd: x"));
  CHECK(has_reply_prefix("Re[2] x"));
  CHECK(has_reply_prefix("回复：x"));
  CHECK(has_reply_prefix("答复:x"));
  CHECK(has_reply_prefix("转发：x"));
  CHECK(has_reply_prefix("回覆：x"));
  CHECK(has_reply_prefix("轉寄：x"));
  CHECK(has_reply_prefix("[ops] Re: x"));
  CHECK_FALSE(has_reply_prefix("周报"));
  CHECK_FALSE(has_reply_prefix("[ops] 周报"));
  CHECK_FALSE(has_reply_prefix("Re x"));
  CHECK_FALSE(has_reply_prefix("Reply needed"));
  CHECK_FALSE(has_reply_prefix(""));
}

TEST_CASE("assign_thread: forward reference join", "[threading]") {
  Fx f;
  const auto a = f.add(f.in_msg("Plan", "a1@x"));
  auto b = f.in_msg("Re: Plan", "b1@x", {"a1@x"}, kT0 + 1000);
  b.from = f.me;
  b.to = {f.carol};
  b.direction = "out";
  const auto rb = f.add(b);
  CHECK(rb.thread_id == a.thread_id);
  // References join regardless of the subject.
  const auto c = f.add(f.in_msg("Totally different", "c1@x", {"<b1@x>"}, kT0 + 2000));
  CHECK(c.thread_id == a.thread_id);
  CHECK(f.agg(a.thread_id).msg_count == 3);
}

TEST_CASE("assign_thread: reply that arrived first is adopted (reverse direction)", "[threading]") {
  Fx f;
  // The reply references a1@x, which we have not seen yet.
  const auto reply = f.add(f.in_msg("Re: Plan", "r1@x", {"a1@x"}, kT0 + 1000));
  // The original arrives later: it joins the reply's thread.
  const auto orig = f.add(f.in_msg("Plan", "a1@x", {}, kT0));
  CHECK(orig.thread_id == reply.thread_id);
  const Agg g = f.agg(orig.thread_id);
  CHECK(g.msg_count == 2);
  CHECK(g.subject == "Plan");  // earliest message's subject
  CHECK(g.last_message_id == reply.message_id);
}

TEST_CASE("adopt_referencing: late Message-ID capture merges threads (B2)", "[threading]") {
  Fx f;
  // Our own sent message whose Message-ID is not known yet.
  Msg out;
  out.owner = f.alice;
  out.direction = "out";
  out.from = f.me;
  out.to = {f.carol};
  out.subject = "Proposal";
  out.is_read = true;
  out.in_inbox = false;
  const auto sent = f.add(out);
  // An auto-responder with an unrelated subject only has the reference, which is still
  // unknown: it lands in a separate thread until the Message-ID is captured.
  const auto reply2 = f.add(f.in_msg("Automatic reply", "r2@x", {"sent-id@resend"}, kT0 + 2000));
  REQUIRE(reply2.thread_id != sent.thread_id);
  f.ts.notifier.clear();
  const int absorbed = f.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE messages SET message_id_header = 'sent-id@resend' WHERE id = ?", sent.message_id);
    return adopt_referencing(tx, f.alice, sent.thread_id, "<sent-id@resend>");
  });
  CHECK(absorbed == 1);
  CHECK(f.thread(reply2.message_id) == sent.thread_id);
  CHECK_FALSE(f.thread_exists(reply2.thread_id));
  CHECK(f.agg(sent.thread_id).msg_count == 2);
  // Later replies now join through the known Message-ID directly.
  const auto reply = f.add(f.in_msg("Something else", "r@x", {"sent-id@resend"}, kT0 + 3000));
  CHECK(reply.thread_id == sent.thread_id);
  CHECK(f.agg(sent.thread_id).msg_count == 3);
  const auto evs = f.ts.notifier.events_of(ws::events::kThreadsChanged);
  REQUIRE_FALSE(evs.empty());
  CHECK(evs[0].user_id == f.alice);
  // Idempotent: nothing left to adopt.
  CHECK(f.ts.db.write([&](db::Tx& tx) { return adopt_referencing(tx, f.alice, sent.thread_id, "sent-id@resend"); }) ==
        0);
  CHECK(f.ts.db.write([&](db::Tx& tx) { return adopt_referencing(tx, f.alice, sent.thread_id, ""); }) == 0);
}

TEST_CASE("assign_thread: siblings sharing a missing ancestor join", "[threading]") {
  Fx f;
  const auto c1 = f.add(f.in_msg("Topic A", "c1@x", {"root@x"}, kT0));
  const auto c2 = f.add(f.in_msg("Topic B", "c2@x", {"root@x"}, kT0 + 1000));
  CHECK(c1.thread_id == c2.thread_id);
}

TEST_CASE("assign_thread: references spanning two threads merge them", "[threading]") {
  Fx f;
  const auto a = f.add(f.in_msg("One", "a@x", {}, kT0));
  const auto b = f.add(f.in_msg("Two", "b@x", {}, kT0 + 1000));
  REQUIRE(a.thread_id != b.thread_id);
  const auto c = f.add(f.in_msg("Three", "c@x", {"a@x", "b@x"}, kT0 + 2000));
  const int64_t keep = std::min(a.thread_id, b.thread_id);
  CHECK(c.thread_id == keep);
  CHECK(f.thread(a.message_id) == keep);
  CHECK(f.thread(b.message_id) == keep);
  CHECK_FALSE(f.thread_exists(std::max(a.thread_id, b.thread_id)));
  CHECK(f.agg(keep).msg_count == 3);
}

TEST_CASE("assign_thread: subject fallback (C7)", "[threading]") {
  SECTION("positive: reply prefix, no refs, within 7 days, shared participant") {
    Fx f;
    const auto a = f.add(f.in_msg("周报", std::nullopt, {}, kT0));
    const auto b = f.add(f.in_msg("回复：周报", std::nullopt, {}, kT0 + 2 * kDay));
    CHECK(b.thread_id == a.thread_id);
  }
  SECTION("positive: unknown refs do not prevent the fallback") {
    Fx f;
    const auto a = f.add(f.in_msg("Budget", std::nullopt, {}, kT0));
    const auto b = f.add(f.in_msg("Re: Budget", "x@y", {"never-seen@z"}, kT0 + kDay));
    CHECK(b.thread_id == a.thread_id);
  }
  SECTION("negative: weekly 周报 from the same person without a reply prefix") {
    Fx f;
    const auto w1 = f.add(f.in_msg("周报", "w1@x", {}, kT0));
    const auto w2 = f.add(f.in_msg("周报", "w2@x", {}, kT0 + 7 * kDay - 1));
    CHECK(w2.thread_id != w1.thread_id);
  }
  SECTION("negative: older than 7 days") {
    Fx f;
    const auto a = f.add(f.in_msg("Budget", std::nullopt, {}, kT0));
    const auto b = f.add(f.in_msg("Re: Budget", std::nullopt, {}, kT0 + 7 * kDay + 1));
    CHECK(b.thread_id != a.thread_id);
  }
  SECTION("negative: no overlap except the owner herself") {
    Fx f;
    const auto a = f.add(f.in_msg("Budget", std::nullopt, {}, kT0));
    Msg other = f.in_msg("Re: Budget", std::nullopt, {}, kT0 + kDay);
    other.from = f.dave;  // only alice (the owner) is common
    const auto b = f.add(other);
    CHECK(b.thread_id != a.thread_id);
  }
  SECTION("negative: different normalized subject") {
    Fx f;
    const auto a = f.add(f.in_msg("Budget 2026", std::nullopt, {}, kT0));
    const auto b = f.add(f.in_msg("Re: Budget 2027", std::nullopt, {}, kT0 + kDay));
    CHECK(b.thread_id != a.thread_id);
  }
  SECTION("negative: list tags distinguish threads") {
    Fx f;
    const auto a = f.add(f.in_msg("[team-a] 周报", std::nullopt, {}, kT0));
    const auto b = f.add(f.in_msg("Re: [team-b] 周报", std::nullopt, {}, kT0 + kDay));
    CHECK(b.thread_id != a.thread_id);
    const auto c = f.add(f.in_msg("Re: [team-a] 周报", std::nullopt, {}, kT0 + kDay));
    CHECK(c.thread_id == a.thread_id);
  }
  SECTION("negative: a thread that only has trashed mail is not a candidate") {
    Fx f;
    Msg t = f.in_msg("Budget", std::nullopt, {}, kT0);
    t.trashed_at = kT0;
    const auto a = f.add(t);
    const auto b = f.add(f.in_msg("Re: Budget", std::nullopt, {}, kT0 + kDay));
    CHECK(b.thread_id != a.thread_id);
  }
}

TEST_CASE("assign_thread: owner isolation (IDOR)", "[threading]") {
  Fx f;
  Msg bobs = f.in_msg("Secret", "s@x");
  bobs.owner = f.bob;
  const auto b = f.add(bobs);
  const auto a = f.add(f.in_msg("Re: Secret", "r@x", {"s@x"}, kT0 + 1000));
  CHECK(a.thread_id != b.thread_id);
  f.ts.db.read([&](db::Conn& c) {
    const std::vector<std::string> refs{"s@x"};
    // Alice only finds her own reply (which references s@x), never Bob's message.
    CHECK(threads_for_refs(c, f.alice, refs) == std::vector<int64_t>{a.thread_id});
    CHECK(threads_for_refs(c, f.bob, refs) == std::vector<int64_t>{b.thread_id});
    CHECK(threads_for_refs(c, f.bob, std::vector<std::string>{"r@x"}).empty());
  });
  // merge_threads refuses threads of another owner.
  CHECK_THROWS_AS(f.ts.db.write([&](db::Tx& tx) { return merge_threads(tx, f.alice, a.thread_id, b.thread_id); }),
                  std::invalid_argument);
  CHECK(f.thread(b.message_id) == b.thread_id);
}

TEST_CASE("threads_for_refs: by Message-ID and by shared reference", "[threading]") {
  Fx f;
  const auto a = f.add(f.in_msg("A", "a@x"));
  const auto b = f.add(f.in_msg("B", "b@x", {"root@x"}, kT0 + 1));
  f.ts.db.read([&](db::Conn& c) {
    std::vector<std::string> refs{"<a@x>", "root@x", "", "a@x"};
    auto ts = threads_for_refs(c, f.alice, refs);
    std::vector<int64_t> want{a.thread_id, b.thread_id};
    std::sort(want.begin(), want.end());
    CHECK(ts == want);
    CHECK(threads_for_refs(c, f.alice, std::vector<std::string>{}).empty());
    CHECK(threads_for_refs(c, f.alice, std::vector<std::string>{"nope@x"}).empty());
  });
}

TEST_CASE("merge_threads", "[threading]") {
  Fx f;
  const auto a = f.add(f.in_msg("A", "a@x", {}, kT0));
  const auto b = f.add(f.in_msg("B", "b@x", {}, kT0 + 5000));
  f.ts.notifier.clear();
  const int64_t kept = f.ts.db.write([&](db::Tx& tx) { return merge_threads(tx, f.alice, a.thread_id, b.thread_id); });
  CHECK(kept == a.thread_id);
  CHECK(f.thread(b.message_id) == a.thread_id);
  CHECK_FALSE(f.thread_exists(b.thread_id));
  const Agg g = f.agg(a.thread_id);
  CHECK(g.msg_count == 2);
  CHECK(g.last_at == kT0 + 5000);
  CHECK(g.last_message_id == b.message_id);
  const auto evs = f.ts.notifier.events_of(ws::events::kThreadsChanged);
  REQUIRE(evs.size() == 1);
  const auto& ids = evs[0].data.at("thread_ids").as_array();
  REQUIRE(ids.size() == 2);
  CHECK(ids[0].as_int64() == a.thread_id);
  CHECK(ids[1].as_int64() == b.thread_id);
  // Same thread: no-op.
  CHECK(f.ts.db.write([&](db::Tx& tx) { return merge_threads(tx, f.alice, a.thread_id, a.thread_id); }) ==
        a.thread_id);
  // Unknown thread id: rejected.
  CHECK_THROWS_AS(f.ts.db.write([&](db::Tx& tx) { return merge_threads(tx, f.alice, a.thread_id, 999999); }),
                  std::invalid_argument);
}

TEST_CASE("recompute_thread: every aggregate from source", "[threading][aggregates]") {
  Fx f;
  int64_t t = 0;
  int64_t sched_ob = 0, sent_ob = 0, canceled_ob = 0, queued_sched_ob = 0;
  f.ts.db.write([&](db::Tx& tx) {
    sent_ob = insert_outbound(tx, f.alice, f.alice_addr, "delivered");
    sched_ob = insert_outbound(tx, f.alice, f.alice_addr, "scheduled", kT0 + 10 * kDay);
    queued_sched_ob = insert_outbound(tx, f.alice, f.alice_addr, "queued", kT0 + 10 * kDay);
    canceled_ob = insert_outbound(tx, f.alice, f.alice_addr, "canceled");
  });
  // 1 unread inbound in inbox, starred
  Msg m1 = f.in_msg("Subject", "m1@x", {}, kT0);
  m1.is_starred = true;
  m1.atts = {Att{}, Att{.filename = "logo.png", .content_type = "image/png", .content_id = "logo@x", .is_inline = true}};
  t = f.add(m1).thread_id;
  auto add_to = [&](Msg m) {
    m.thread_id = t;
    return f.add(m);
  };
  // 2 read inbound archived
  Msg m2 = f.in_msg("Re: Subject", "m2@x", {}, kT0 + 1000);
  m2.is_read = true;
  m2.in_inbox = false;
  add_to(m2);
  // 3 sent (delivered)
  Msg m3;
  m3.owner = f.alice;
  m3.direction = "out";
  m3.from = f.me;
  m3.to = {f.carol};
  m3.subject = "Re: Subject";
  m3.is_read = true;
  m3.in_inbox = false;
  m3.date = kT0 + 2000;
  m3.outbound_id = sent_ob;
  m3.text = "sent text";
  m3.snippet = "sent snippet";
  const auto r3 = add_to(m3);
  // 4 scheduled via resend, 5 queued + scheduled (local), 6 canceled
  Msg m4 = m3;
  m4.outbound_id = sched_ob;
  m4.date = kT0 + 3000;
  add_to(m4);
  Msg m5 = m3;
  m5.outbound_id = queued_sched_ob;
  m5.date = kT0 + 3500;
  add_to(m5);
  Msg m6 = m3;
  m6.outbound_id = canceled_ob;
  m6.date = kT0 + 3600;
  add_to(m6);
  // 7 draft (latest by date)
  Msg m7 = m3;
  m7.outbound_id.reset();
  m7.is_draft = true;
  m7.date = kT0 + 9000;
  m7.atts = {Att{.filename = "draft.zip"}};
  add_to(m7);
  // 8 spam unread, 9 trashed unread
  Msg m8 = f.in_msg("Re: Subject", "m8@x", {}, kT0 + 4000);
  m8.is_spam = true;
  m8.in_inbox = false;
  add_to(m8);
  Msg m9 = f.in_msg("Re: Subject", "m9@x", {}, kT0 + 5000);
  m9.trashed_at = kT0 + 6000;
  add_to(m9);

  const Agg g = f.agg(t);
  CHECK(g.msg_count == 6);        // m1..m6 (normal, not draft)
  CHECK(g.unread_count == 1);     // m1
  CHECK(g.inbox_count == 1);      // m1
  CHECK(g.inbox_unread == 1);
  CHECK(g.starred_count == 1);
  CHECK(g.sent_count == 1);       // m3 only: scheduled/canceled/queued+scheduled_at excluded
  CHECK(g.scheduled_count == 2);  // m4 (scheduled) + m5 (queued with scheduled_at)
  CHECK(g.draft_count == 1);
  CHECK(g.spam_count == 1);
  CHECK(g.spam_unread == 1);
  CHECK(g.trash_count == 1);
  CHECK(g.attachment_count == 1);  // m1's pdf; inline image and the draft's file excluded
  CHECK(g.last_at == kT0 + 9000);  // includes the draft
  CHECK(g.spam_last_at == kT0 + 4000);
  CHECK(g.trash_last_at == kT0 + 5000);
  CHECK(g.subject == "Subject");
  CHECK(g.norm_subject == "subject");
  // Snippet / last message: latest normal non-draft (m6, the canceled one at +3600).
  CHECK(g.last_message_id.has_value());
  (void)r3;

  // Participants: carol (unread from m1/m8/m9) then alice.
  const auto pj = boost::json::parse(g.participants_json).as_array();
  REQUIRE(pj.size() == 2);
  CHECK(pj[0].at("email") == "carol@ext.example");
  CHECK(pj[0].at("unread") == true);
  CHECK(pj[1].at("email") == "alice@team.example");
  CHECK(pj[1].at("unread") == false);
}

TEST_CASE("recompute_thread: snippet source, drafts-only threads, deletion", "[threading][aggregates]") {
  Fx f;
  Msg a = f.in_msg("S", "a@x", {}, kT0);
  a.snippet = "first";
  const auto ra = f.add(a);
  Msg b = f.in_msg("Re: S", "b@x", {"a@x"}, kT0 + 1000);
  b.snippet = "second";
  const auto rb = f.add(b);
  Agg g = f.agg(ra.thread_id);
  CHECK(g.snippet == "second");
  CHECK(g.last_message_id == rb.message_id);

  // A newer draft does not become the snippet.
  Msg d;
  d.owner = f.alice;
  d.thread_id = ra.thread_id;
  d.direction = "out";
  d.is_draft = true;
  d.from = f.me;
  d.subject = "Re: S";
  d.snippet = "draft text";
  d.date = kT0 + 5000;
  const auto rd = f.add(d);
  g = f.agg(ra.thread_id);
  CHECK(g.snippet == "second");
  CHECK(g.last_at == kT0 + 5000);

  // Drafts-only thread: the draft provides snippet, subject and participant.
  Msg solo = d;
  solo.thread_id.reset();
  solo.subject = "New idea";
  solo.snippet = "only a draft";
  const auto rs = f.add(solo);
  g = f.agg(rs.thread_id);
  CHECK(g.msg_count == 0);
  CHECK(g.draft_count == 1);
  CHECK(g.snippet == "only a draft");
  CHECK(g.subject == "New idea");
  CHECK(boost::json::parse(g.participants_json).as_array().size() == 1);

  // Deleting every message deletes the thread.
  const bool kept = f.ts.db.write([&](db::Tx& tx) {
    tx.run("DELETE FROM messages WHERE id = ?", rs.message_id);
    return recompute_thread(tx, f.alice, rs.thread_id);
  });
  CHECK_FALSE(kept);
  CHECK_FALSE(f.thread_exists(rs.thread_id));
  // Another owner cannot delete or touch alice's thread.
  CHECK_FALSE(f.ts.db.write([&](db::Tx& tx) { return recompute_thread(tx, f.bob, ra.thread_id); }));
  CHECK(f.thread_exists(ra.thread_id));
  (void)rd;
}

TEST_CASE("recompute_thread: participants are capped at 6", "[threading][aggregates]") {
  Fx f;
  const auto first = f.add(f.in_msg("Big", "p0@x", {}, kT0));
  for (int i = 1; i <= 8; ++i) {
    Msg m = f.in_msg("Re: Big", "p" + std::to_string(i) + "@x", {"p0@x"}, kT0 + i * 1000);
    m.from = {"P" + std::to_string(i), "p" + std::to_string(i) + "@ext.example"};
    m.is_read = i % 2 == 0;
    f.add(m);
  }
  const auto pj = boost::json::parse(f.agg(first.thread_id).participants_json).as_array();
  REQUIRE(pj.size() == 6);
  CHECK(pj[0].at("email") == "carol@ext.example");  // first participant always kept
  CHECK(pj[1].at("email") == "p4@ext.example");     // then the 5 most recent, in order
  CHECK(pj[5].at("email") == "p8@ext.example");
  CHECK(pj[5].at("name") == "P8");
  CHECK(pj[5].at("unread") == false);
  CHECK(pj[4].at("unread") == true);  // p7 is unread
}
