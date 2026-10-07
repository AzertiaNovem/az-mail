// Owner: WP-B — inbound routing and delivery: envelope vs headers (forged To), +tags, alias
// fan-out, BCC privacy for every recipient role, loopback merge with and without X-AzMail-Ref,
// split-delivery dedupe, idempotent re-delivery, unroutable mail, spam rules, attachments,
// inbound_emails state, IDOR.
#include "send_fixtures.hpp"

#include "ws/events.hpp"

#include <boost/json/parse.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::sendfx;

namespace {

std::set<int64_t> owners(const DeliveryResult& r) {
  std::set<int64_t> out;
  for (const auto& c : r.copies) out.insert(c.owner_id);
  return out;
}

const DeliveredCopy& copy_of(const DeliveryResult& r, int64_t owner) {
  for (const auto& c : r.copies)
    if (c.owner_id == owner) return c;
  FAIL("no copy for owner " << owner);
  return r.copies.front();
}

}  // namespace

TEST_CASE("routing: the envelope wins; a forged To delivers nothing (C1)", "[delivery][routing]") {
  SendFx fx;
  // Header says carol, envelope says bob.
  const auto r = fx.deliver(SendFx::inbound("rcv-1", kCustomer, {kCarol}, {"bob@team.example"}, "你好"));
  CHECK(r.state == DeliveryResult::State::Delivered);
  CHECK(owners(r) == std::set<int64_t>{fx.bob});
  CHECK(fx.message_ids(fx.carol).empty());
  const auto v = fx.message(copy_of(r, fx.bob).message_id, fx.bob).value();
  CHECK(v.to == std::vector<Address>{kCarol});  // headers are shown as received
  CHECK(v.delivered_to == "bob@team.example");
  CHECK(v.bcc == std::vector<Address>{{"", "bob@team.example"}});  // not in To/Cc → "密送给我"
  CHECK(v.in_inbox);
  CHECK_FALSE(v.is_read);
  CHECK(v.auth->dmarc == "pass");

  // Without an envelope, To ∪ Cc decide; local parts are case-insensitive and +tags stripped.
  auto e = SendFx::inbound("rcv-2", kCustomer, {{"", "Carol+News@TEAM.example"}}, {}, "无信封");
  e.cc = {{"", "dave@team.example"}, kExternal, {"", "ghost@team.example"}};
  const auto r2 = fx.deliver(e);
  CHECK(owners(r2) == (std::set<int64_t>{fx.carol, fx.dave}));
  CHECK(copy_of(r2, fx.carol).delivered_to == "carol@team.example");
  CHECK(fx.message(copy_of(r2, fx.carol).message_id, fx.carol)->bcc.empty());

  CHECK(fx.ts.db.read([&](db::Conn& c) {
          const std::vector<std::string> rcpts{"BOB+x@team.example", "support@team.example", "nobody@team.example",
                                               "x@other.example", "Alice@Team.Example"};
          return resolve_local_recipients(c, rcpts).size();
        }) == 2);  // bob, then alice through support@ (bob deduped)
}

TEST_CASE("alias fan-out: each member gets an independent copy (C3)", "[delivery][alias]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("INSERT INTO alias_members(alias_id, user_id, can_send_as) VALUES(?, ?, 0)", fx.support, fx.dave);
    tx.run("UPDATE users SET disabled = 1 WHERE id = ?", fx.dave);
  });
  fx.ts.notifier.clear();
  const auto r = fx.deliver(SendFx::inbound("rcv-a", kCustomer, {kSupport}, {"support@team.example"}, "咨询",
                                            "q1@customer.example"));
  CHECK(owners(r) == (std::set<int64_t>{fx.alice, fx.bob}));  // dave is disabled
  for (int64_t owner : {fx.alice, fx.bob}) {
    const auto& c = copy_of(r, owner);
    CHECK(c.delivered_to == "support@team.example");
    const auto v = fx.message(c.message_id, owner).value();
    CHECK(v.delivered_to == "support@team.example");
    CHECK(v.bcc.empty());  // the alias was in To
    const auto news = fx.events_of(ws::events::kMailNew, owner);
    REQUIRE(news.size() == 1);
    CHECK(news[0].data.at("message_id").as_int64() == c.message_id);
    CHECK(news[0].data.at("thread_id").as_int64() == c.thread_id);
    CHECK(news[0].data.at("subject").as_string() == "咨询");
    CHECK(news[0].data.at("from").as_object().at("email").as_string() == "wang@customer.example");
    CHECK(news[0].data.at("in_inbox").as_bool());
    CHECK_FALSE(news[0].data.at("is_spam").as_bool());
  }
  // Independent read state.
  const int64_t alice_msg = copy_of(r, fx.alice).message_id;
  fx.ts.db.write([&](db::Tx& tx) { patch_message(tx, fx.alice, alice_msg, MessagePatch{true, {}, {}, {}}); });
  CHECK(fx.message(alice_msg)->is_read);
  CHECK_FALSE(fx.message(copy_of(r, fx.bob).message_id, fx.bob)->is_read);
  // A user reached directly and through the alias gets one copy (first match wins).
  const auto r2 = fx.deliver(SendFx::inbound("rcv-b", kCustomer, {kAlice, kSupport},
                                             {"alice@team.example", "support@team.example"}, "两次"));
  CHECK(r2.copies.size() == 2);
  CHECK(copy_of(r2, fx.alice).delivered_to == "alice@team.example");
  CHECK(copy_of(r2, fx.bob).delivered_to == "support@team.example");
}

TEST_CASE("BCC privacy for sender, To, Cc and Bcc recipients (C2)", "[delivery][bcc]") {
  SendFx fx;
  // alice → To bob, Cc carol, Bcc dave (E2E 09): first the sender's copy.
  DraftInput in;
  in.to = std::vector<Address>{kBob};
  in.cc = std::vector<Address>{kCarol};
  in.bcc = std::vector<Address>{kDave};
  in.subject = "BCC 测试";
  const SendResult s = fx.send(fx.create(in));
  fx.accept(s.outbound_id, "re_bcc");
  CHECK(fx.message(s.message_id)->bcc == std::vector<Address>{kDave});

  // The loopback: Resend's received email carries bcc[], but InboundEmail never does; the
  // envelope lists all three local recipients.
  auto e = SendFx::inbound("rcv-bcc", kAlice, {kBob}, {"bob@team.example", "carol@team.example", "dave@team.example"},
                           "BCC 测试", "bcc@resend.dev");
  e.cc = {kCarol};
  e.x_azmail_ref = fx.outbound(s.outbound_id).uuid;
  const auto r = fx.deliver(e);
  CHECK(owners(r) == (std::set<int64_t>{fx.bob, fx.carol, fx.dave}));
  CHECK(fx.message(copy_of(r, fx.bob).message_id, fx.bob)->bcc.empty());
  CHECK(fx.message(copy_of(r, fx.carol).message_id, fx.carol)->bcc.empty());
  CHECK(fx.message(copy_of(r, fx.dave).message_id, fx.dave)->bcc == std::vector<Address>{{"", "dave@team.example"}});
  // BCC is never searchable by the recipients (only the sender's copy indexes it).
  for (const auto& c : r.copies) {
    const std::string to_text = fx.text_of("SELECT to_text FROM message_fts WHERE rowid = ?", c.message_id);
    CHECK(to_text.find("dave") == std::string::npos);
  }
  // The loopback captured the outbound's Message-ID (B2 source c).
  CHECK(fx.outbound(s.outbound_id).message_id_header == "bcc@resend.dev");
  CHECK(fx.message(s.message_id)->message_id_header == "bcc@resend.dev");
}

TEST_CASE("loopback merge with X-AzMail-Ref: no duplicate for owners holding a copy (C3)", "[delivery][loopback]") {
  SendFx fx;
  // alice as support@ → To support@ and bob@: alice holds the sent copy, bob the shared copy.
  DraftInput in;
  in.to = std::vector<Address>{kSupport, kBob};
  in.from_address_id = fx.support;
  in.subject = "内部通知";
  const SendResult s = fx.send(fx.create(in));
  fx.accept(s.outbound_id, "re_loop");
  const int64_t bob_shared =
      fx.scalar_of("SELECT id FROM messages WHERE owner_id = ? AND outbound_id = ?", fx.bob, s.outbound_id);
  CHECK(fx.folder_ids(Folder::Inbox).empty());

  auto e = SendFx::inbound("rcv-loop", kSupport, {kSupport, kBob}, {"support@team.example", "bob@team.example"},
                           "内部通知", "loop@resend.dev");
  e.x_azmail_ref = fx.outbound(s.outbound_id).uuid;
  e.auth = AuthResults{"pass", "fail", "none"};  // no DKIM/DMARC pass: the matching ref proves it is ours
  fx.ts.notifier.clear();
  const auto r = fx.deliver(e);
  CHECK(r.state == DeliveryResult::State::Delivered);
  REQUIRE(r.copies.size() == 2);
  for (const auto& c : r.copies) CHECK(c.loopback_merged);
  CHECK(copy_of(r, fx.alice).message_id == s.message_id);
  CHECK(copy_of(r, fx.bob).message_id == bob_shared);
  CHECK(fx.scalar("SELECT COUNT(*) FROM messages WHERE direction = 'in'") == 0);  // nothing duplicated
  CHECK(fx.message(s.message_id)->in_inbox);
  CHECK(fx.message(s.message_id)->is_read);  // the sender has read their own mail
  const auto bob_view = fx.message(bob_shared, fx.bob).value();
  CHECK(bob_view.in_inbox);
  CHECK_FALSE(bob_view.is_read);  // for bob it is new mail
  CHECK(fx.folder_ids(Folder::Inbox) == std::vector<int64_t>{s.thread_id});
  CHECK(fx.events_of(ws::events::kMailNew, fx.bob).size() == 1);
  CHECK(fx.events_of(ws::events::kMailNew, fx.alice).empty());
  CHECK(fx.outbound(s.outbound_id).message_id_header == "loop@resend.dev");
  // Re-delivery of the same email is a no-op.
  CHECK(fx.deliver(e).state == DeliveryResult::State::Duplicate);
}

TEST_CASE("loopback merge without X-AzMail-Ref, by Message-ID (E2E 08, §H 27)", "[delivery][loopback]") {
  SendFx fx;
  const SendResult s = fx.send(fx.simple_draft({kAlice}, "备忘"));
  fx.accept(s.outbound_id, "re_memo");
  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, s.outbound_id, "memo@resend.dev"); });
  const auto r = fx.deliver(SendFx::inbound("rcv-memo", kAlice, {kAlice}, {"alice@team.example"}, "备忘",
                                            "memo@resend.dev"));
  REQUIRE(r.copies.size() == 1);
  CHECK(r.copies[0].loopback_merged);
  CHECK(r.copies[0].message_id == s.message_id);
  CHECK(fx.message_ids(fx.alice) == std::vector<int64_t>{s.message_id});
  CHECK(fx.message(s.message_id)->in_inbox);

  // A forged mail reusing our Message-ID / X-AzMail-Ref under another From is NOT our loopback.
  auto forged = SendFx::inbound("rcv-forged", kExternal, {kAlice}, {"alice@team.example"}, "备忘", "memo@resend.dev");
  forged.x_azmail_ref = fx.outbound(s.outbound_id).uuid;
  const auto rf = fx.deliver(forged);
  REQUIRE(rf.copies.size() == 1);
  CHECK_FALSE(rf.copies[0].loopback_merged);
  CHECK(rf.copies[0].message_id != s.message_id);
  // ... and with our From but a foreign ref it is spoofed internal mail.
  auto spoof = SendFx::inbound("rcv-spoof", kAlice, {kBob}, {"bob@team.example"}, "转账", "spoof@evil.example");
  spoof.x_azmail_ref = "00000000-0000-4000-8000-000000000000";
  spoof.auth = AuthResults{"fail", "fail", "none"};
  const auto rs = fx.deliver(spoof);
  REQUIRE(rs.copies.size() == 1);
  CHECK(rs.copies[0].is_spam);
  CHECK(fx.message(rs.copies[0].message_id, fx.bob)->warnings == std::vector<std::string>{"spoofed_internal"});
}

TEST_CASE("internal reply threads with the sender's message (both directions)", "[delivery][threading]") {
  SendFx fx;
  const SendResult s = fx.send(fx.simple_draft({kBob}, "季度计划"));
  fx.accept(s.outbound_id, "re_plan");
  auto first = SendFx::inbound("rcv-plan", kAlice, {kBob}, {"bob@team.example"}, "季度计划", "plan@resend.dev");
  first.x_azmail_ref = fx.outbound(s.outbound_id).uuid;
  const auto bob_copy = fx.deliver(first).copies.at(0);
  // bob replies (via the external route for brevity): alice's thread gets it via In-Reply-To.
  auto reply = SendFx::inbound("rcv-re", kBob, {kAlice}, {"alice@team.example"}, "Re: 季度计划", "re@resend.dev");
  reply.in_reply_to = "plan@resend.dev";
  reply.references = {"plan@resend.dev"};
  const auto r = fx.deliver(reply);
  CHECK(copy_of(r, fx.alice).thread_id == s.thread_id);
  CHECK(fx.thread(s.thread_id)->messages.size() == 2);
  (void)bob_copy;
}

TEST_CASE("split delivery and re-delivery are idempotent (C8)", "[delivery][idempotency]") {
  SendFx fx;
  // Two Resend emails for the same SMTP message (same Message-ID), overlapping envelopes.
  auto a = SendFx::inbound("rcv-split-1", kCustomer, {kAlice, kBob}, {"alice@team.example"}, "分批", "split@x");
  auto b = SendFx::inbound("rcv-split-2", kCustomer, {kAlice, kBob}, {"alice@team.example", "bob@team.example"},
                           "分批", "split@x");
  const auto ra = fx.deliver(a);
  CHECK(owners(ra) == std::set<int64_t>{fx.alice});
  const auto rb = fx.deliver(b);
  CHECK(rb.state == DeliveryResult::State::Delivered);
  CHECK(owners(rb) == std::set<int64_t>{fx.bob});  // alice's duplicate skipped
  CHECK(fx.message_ids(fx.alice).size() == 1);
  CHECK(fx.message_ids(fx.bob).size() == 1);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM threads WHERE owner_id = ?", fx.alice) == 1);  // no empty thread left
  CHECK(fx.scalar("SELECT COUNT(*) FROM threads t WHERE msg_count + draft_count + spam_count + trash_count = 0") == 0);

  // A third split part with nothing new: delivered, but a duplicate.
  auto c = SendFx::inbound("rcv-split-3", kCustomer, {kAlice}, {"alice@team.example"}, "分批", "split@x");
  const auto rc = fx.deliver(c);
  CHECK(rc.state == DeliveryResult::State::Duplicate);
  CHECK(fx.text_of("SELECT state FROM inbound_emails WHERE resend_id = 'rcv-split-3'") == "delivered");

  // Re-running a delivered email changes nothing (no events, no rows).
  fx.ts.notifier.clear();
  const int64_t before = fx.scalar("SELECT COUNT(*) FROM messages");
  const auto again = fx.deliver(a);
  CHECK(again.state == DeliveryResult::State::Duplicate);
  CHECK(again.inbound_id == ra.inbound_id);
  CHECK(again.copies.empty());
  CHECK(fx.scalar("SELECT COUNT(*) FROM messages") == before);
  CHECK(fx.ts.notifier.size() == 0);
}

TEST_CASE("unroutable mail and the deliver-to-admins policy (B9, C4)", "[delivery][unroutable]") {
  SendFx fx;
  const auto r = fx.deliver(SendFx::inbound("rcv-u1", kCustomer, {kExternal}, {"ext@other.example"}, "别人的邮件"));
  CHECK(r.state == DeliveryResult::State::Unroutable);
  CHECK(r.copies.empty());
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-u1"); }) == InboundState::Unroutable);
  CHECK(fx.scalar("SELECT COUNT(*) FROM messages") == 0);
  const std::string rcpts = fx.text_of("SELECT recipients_json FROM inbound_emails WHERE resend_id = 'rcv-u1'");
  CHECK(boost::json::parse(rcpts).as_array().at(0).as_string() == "ext@other.example");

  const auto lost = SendFx::inbound("rcv-u2", kCustomer, {{"", "ghost@team.example"}}, {"ghost@team.example"}, "幽灵");
  CHECK(fx.deliver(lost).state == DeliveryResult::State::Unroutable);
  // Admin retry with the policy on: the admins (alice) receive it.
  DeliveryOptions o;
  o.unroutable_to_admins = true;
  const auto r2 = fx.deliver(lost, o);
  CHECK(r2.state == DeliveryResult::State::Delivered);
  CHECK(owners(r2) == std::set<int64_t>{fx.alice});
  CHECK(copy_of(r2, fx.alice).delivered_to == "ghost@team.example");
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-u2"); }) == InboundState::Delivered);
  // The policy never applies to foreign domains.
  CHECK(fx.deliver(SendFx::inbound("rcv-u3", kCustomer, {kExternal}, {"ext@other.example"}), o).state ==
        DeliveryResult::State::Unroutable);
}

TEST_CASE("spam rules (C11) and the pure spam_warnings table", "[delivery][spam]") {
  CHECK(spam_warnings({"pass", "pass", "pass"}, true, false).empty());
  CHECK(spam_warnings({"pass", "pass", "FAIL"}, false, false) == std::vector<std::string>{"dmarc_fail"});
  CHECK(spam_warnings({"fail", "fail", "fail"}, true, false) ==
        (std::vector<std::string>{"dmarc_fail", "spoofed_internal"}));
  CHECK(spam_warnings({"pass", "pass", "none"}, true, false).empty());   // DKIM pass is enough
  CHECK(spam_warnings({"pass", "none", "pass"}, true, false).empty());   // DMARC pass is enough
  CHECK(spam_warnings({std::nullopt, std::nullopt, std::nullopt}, true, true).empty());  // our loopback
  CHECK(spam_warnings({std::nullopt, std::nullopt, std::nullopt}, true, false) ==
        std::vector<std::string>{"spoofed_internal"});
  CHECK(spam_warnings({std::nullopt, std::nullopt, std::nullopt}, false, false).empty());

  SendFx fx;
  auto e = SendFx::inbound("rcv-spam", kCustomer, {kBob}, {"bob@team.example"}, "中奖通知");
  e.auth = AuthResults{"pass", "pass", "fail"};
  fx.ts.notifier.clear();
  const auto r = fx.deliver(e);
  const auto& c = copy_of(r, fx.bob);
  CHECK(c.is_spam);
  CHECK_FALSE(c.in_inbox);
  const auto v = fx.message(c.message_id, fx.bob).value();
  CHECK(v.is_spam);
  CHECK_FALSE(v.in_inbox);
  CHECK(v.warnings == std::vector<std::string>{"dmarc_fail"});
  CHECK(fx.folder_ids(Folder::Spam, fx.bob) == std::vector<int64_t>{c.thread_id});
  CHECK(fx.folder_ids(Folder::Inbox, fx.bob).empty());
  CHECK(fx.events_of(ws::events::kMailNew, fx.bob).at(0).data.at("is_spam").as_bool());
  CHECK(fx.scalar("SELECT COUNT(*) FROM contacts") == 0);  // spam senders don't become contacts
  // not_spam moves it to the inbox (E2E 24).
  fx.ts.db.write([&](db::Tx& tx) {
    const std::vector<int64_t> ids{c.thread_id};
    apply_thread_action(tx, fx.bob, ids, ThreadAction::NotSpam, std::nullopt);
  });
  CHECK(fx.folder_ids(Folder::Inbox, fx.bob) == std::vector<int64_t>{c.thread_id});
}

TEST_CASE("bodies, attachments, references, contacts and inbound metadata", "[delivery]") {
  SendFx fx;
  auto e = SendFx::inbound("rcv-full", {"王五", "Wang@Customer.Example"}, {kBob}, {"bob@team.example"}, "资料",
                           "<full@customer.example>");
  e.html = "<p>见附件</p><img src=\"cid:img1\">";
  e.text = std::nullopt;
  e.references = {"<a@x>", "b@x"};
  e.in_reply_to = "<b@x>";
  e.raw = BlobRef{std::string(64, 'a'), 4321, "local"};
  e.attachments = {
      InboundAttachment{"att_1", BlobRef{std::string(64, 'b'), 10, "local"}, "../../etc/合同.pdf", "Application/PDF",
                        "attachment", std::nullopt},
      InboundAttachment{"att_2", BlobRef{std::string(64, 'c'), 20, "r2"}, "logo.png", "image/png", "attachment",
                        std::string("<img1>")},
      InboundAttachment{"att_3", BlobRef{std::string(64, 'd'), 30, "local"}, "", "bogus", "inline", std::nullopt}};
  const auto r = fx.deliver(e);
  const auto& c = copy_of(r, fx.bob);
  const auto v = fx.message(c.message_id, fx.bob).value();
  CHECK(v.message_id_header == "full@customer.example");
  CHECK(v.snippet == "见附件");
  REQUIRE(v.attachments.size() == 3);
  CHECK(v.attachments[0].filename == "合同.pdf");  // path stripped
  CHECK(v.attachments[0].content_type == "application/pdf");
  CHECK_FALSE(v.attachments[0].is_inline);
  CHECK(v.attachments[1].is_inline);  // referenced by cid: although the disposition says attachment
  CHECK(v.attachments[1].content_id == "img1");
  CHECK(v.attachments[2].filename == "attachment");
  CHECK(v.attachments[2].content_type == "application/octet-stream");
  CHECK_FALSE(v.attachments[2].is_inline);  // inline disposition without a Content-ID
  REQUIRE(v.html);
  CHECK(v.html->find("/api/files/" + std::to_string(v.attachments[1].id) + "?d=i") != std::string::npos);
  REQUIRE(v.raw_url);
  CHECK(fx.text_of("SELECT storage FROM blobs WHERE sha256 = ?", std::string(64, 'c')) == "r2");
  CHECK(fx.scalar("SELECT COUNT(*) FROM blobs") == 4);
  CHECK(fx.scalar_of("SELECT has_attachments FROM messages WHERE id = ?", c.message_id) == 1);
  CHECK(fx.scalar_of("SELECT size_bytes FROM messages WHERE id = ?", c.message_id) == 4321);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM message_refs WHERE message_id = ?", c.message_id) == 2);
  CHECK(fx.text_of("SELECT in_reply_to FROM messages WHERE id = ?", c.message_id) == "b@x");
  CHECK(fx.ts.db.read([&](db::Conn& cn) {
          return cn.scalar<double>("SELECT score FROM contacts WHERE owner_id = ? AND email = ?", fx.bob,
                                   std::string("wang@customer.example"));
        }) == 0.2);
  // inbound_emails keeps metadata only (no bodies) and the ordered References chain.
  auto row = fx.ts.db.read([&](db::Conn& cn) {
    auto s = cn.prepare(
        "SELECT state, message_id_header, from_email, subject, raw_sha256, meta_json FROM inbound_emails WHERE resend_id = ?");
    s.bind_all(std::string("rcv-full"));
    REQUIRE(s.step());
    CHECK(s.text(0) == "delivered");
    CHECK(s.text(1) == "full@customer.example");
    CHECK(s.text(2) == "Wang@Customer.Example");
    CHECK(s.text(3) == "资料");
    CHECK(s.text(4) == std::string(64, 'a'));
    return s.text(5);
  });
  const auto meta = boost::json::parse(row).as_object();
  CHECK(meta.at("references").as_array().size() == 2);
  CHECK(meta.at("references").as_array().at(0).as_string() == "a@x");
  CHECK(row.find("见附件") == std::string::npos);
  // FTS: attachment names and body are searchable.
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM message_fts WHERE message_fts MATCH ?", std::string("\"合同.pdf\"")) == 1);
}

TEST_CASE("dates: Date header, received_at fallback, bogus future Date clamped", "[delivery]") {
  SendFx fx;
  const int64_t now = azm::now_ms();
  auto e = SendFx::inbound("rcv-d1", kCustomer, {kBob}, {"bob@team.example"});
  e.date = now + 365LL * 24 * 3600 * 1000;
  e.received_at = now - 5000;
  const auto c1 = copy_of(fx.deliver(e), fx.bob);
  CHECK(fx.scalar_of("SELECT date FROM messages WHERE id = ?", c1.message_id) == now - 5000);
  auto e2 = SendFx::inbound("rcv-d2", kCustomer, {kBob}, {"bob@team.example"});
  e2.date = 0;
  e2.received_at = now - 7000;
  const auto c2 = copy_of(fx.deliver(e2), fx.bob);
  CHECK(fx.scalar_of("SELECT date FROM messages WHERE id = ?", c2.message_id) == now - 7000);
}

TEST_CASE("inbound_emails state machine", "[delivery][state]") {
  SendFx fx;
  const int64_t id = fx.ts.db.write([&](db::Tx& tx) { return record_inbound_pending(tx, "rcv-s", InboundSource::Poll, 0); });
  CHECK(fx.ts.db.write([&](db::Tx& tx) { return record_inbound_pending(tx, "rcv-s", InboundSource::Webhook, 0); }) == id);
  CHECK(fx.text_of("SELECT source FROM inbound_emails WHERE id = ?", id) == "poll");  // first writer wins
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-s"); }) == InboundState::Pending);
  CHECK_FALSE(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "never-seen"); }));

  fx.ts.db.write([&](db::Tx& tx) { mark_inbound_failed(tx, "rcv-s", "receiving 404 ×5", 0); });
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-s"); }) == InboundState::Failed);
  CHECK(fx.text_of("SELECT error FROM inbound_emails WHERE id = ?", id) == "receiving 404 ×5");
  // An admin retry that now succeeds delivers it; a late failure never downgrades that.
  const auto r = fx.deliver(SendFx::inbound("rcv-s", kCustomer, {kBob}, {"bob@team.example"}));
  CHECK(r.state == DeliveryResult::State::Delivered);
  CHECK(r.inbound_id == id);
  fx.ts.db.write([&](db::Tx& tx) { mark_inbound_failed(tx, "rcv-s", "late", 0); });
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-s"); }) == InboundState::Delivered);
  CHECK(fx.text_of("SELECT error FROM inbound_emails WHERE id = ?", id) == "<null>");
  // mark_inbound_failed for an id never recorded creates the row.
  fx.ts.db.write([&](db::Tx& tx) { mark_inbound_failed(tx, "rcv-new", std::string(2000, 'x'), 0); });
  CHECK(fx.ts.db.read([&](db::Conn& c) { return inbound_state(c, "rcv-new"); }) == InboundState::Failed);
  CHECK(fx.text_of("SELECT error FROM inbound_emails WHERE resend_id = 'rcv-new'").size() == 500);
  CHECK_THROWS_AS(fx.deliver(SendFx::inbound("", kCustomer, {kBob}, {"bob@team.example"})), std::invalid_argument);
}

TEST_CASE("delivery IDOR: copies are owner-scoped", "[delivery][idor]") {
  SendFx fx;
  const auto r = fx.deliver(SendFx::inbound("rcv-i", kCustomer, {kBob}, {"bob@team.example"}, "私信"));
  const auto& c = copy_of(r, fx.bob);
  CHECK_FALSE(fx.message(c.message_id, fx.alice));
  CHECK_FALSE(fx.thread(c.thread_id, fx.alice));
  CHECK(fx.ts.db.read([&](db::Conn& cn) { return message_attachments(cn, fx.alice, c.message_id); }).empty());
  CHECK(api_error([&] {
          fx.ts.db.write([&](db::Tx& tx) { patch_message(tx, fx.alice, c.message_id, MessagePatch{true, {}, {}, {}}); });
        }).status == 404);
  // alice cannot reply to bob's message either.
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(c.message_id);
  CHECK(api_error([&] { fx.create(in); }).status == 404);
}
