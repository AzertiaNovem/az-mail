// Owner: WP-B — queue_send (validation, freeze, scheduling decision, shared alias copies),
// load_send_plan, job transitions, cancel / reschedule / retry, late Message-ID capture.
#include "send_fixtures.hpp"

#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "ws/events.hpp"

#include <boost/json/parse.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::sendfx;
using test::mailfx::Att;
using test::mailfx::Msg;

namespace {

constexpr int64_t kHour = 3'600'000;

std::vector<Address> many(std::size_t n) {
  std::vector<Address> out;
  for (std::size_t i = 0; i < n; ++i) out.push_back({"", "user" + std::to_string(i) + "@ext.example"});
  return out;
}

test::mailfx::Inserted inbound_parent(SendFx& fx, int64_t owner, std::optional<std::string> msgid,
                                      std::vector<std::string> refs = {}, std::string delivered_to = "alice@team.example",
                                      std::optional<int64_t> inbound_id = {}) {
  Msg m;
  m.owner = owner;
  m.from = kCustomer;
  m.to = {kSupport};
  m.subject = "咨询";
  m.message_id = std::move(msgid);
  m.refs = std::move(refs);
  m.delivered_to = std::move(delivered_to);
  m.inbound_id = inbound_id;
  m.date = azm::now_ms() - 60'000;
  return fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_message(tx, m); });
}

Draft reply_to(SendFx& fx, int64_t parent_message_id, int64_t owner = 0, std::string html = "<p>回复</p>") {
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(parent_message_id);
  in.html = std::move(html);
  return fx.create(in, owner);
}

}  // namespace

TEST_CASE("queue_send validation errors", "[outbound][send][validation]") {
  SendFx fx;
  SECTION("no recipients") {
    const ApiError e = api_error([&] { fx.send(fx.simple_draft({})); });
    CHECK(e.status == 422);
    CHECK(e.code == "no_recipients");
  }
  SECTION("too many recipients per field") {
    DraftInput in;
    in.to = many(50);
    in.cc = many(3);
    const Draft ok = fx.create(in);
    CHECK_NOTHROW(fx.send(ok));  // 50 is the limit; duplicates in cc are dropped
    in.to = std::vector<Address>{kBob};
    in.cc = many(51);
    const ApiError e = api_error([&] { fx.send(fx.create(in)); });
    CHECK(e.status == 422);
    CHECK(e.code == "too_many_recipients");
    CHECK(e.details.at("field").as_string() == "cc");
  }
  SECTION("unknown local recipients (C4), incl. disabled users and member-less aliases") {
    fx.ts.db.write([&](db::Tx& tx) {
      tx.run("UPDATE users SET disabled = 1 WHERE id = ?", fx.dave);
      test::seed_alias(tx, "empty@team.example", {});
    });
    DraftInput in;
    in.to = std::vector<Address>{kBob, {"", "Nobody@Team.Example"}, kExternal};
    in.cc = std::vector<Address>{kDave, {"", "empty@team.example"}, {"", "bob+tag@team.example"}};
    const ApiError e = api_error([&] { fx.send(fx.create(in)); });
    CHECK(e.status == 422);
    CHECK(e.code == "unknown_local_recipient");
    std::vector<std::string> emails;
    for (const auto& v : e.details.at("emails").as_array()) emails.emplace_back(v.as_string());
    CHECK(emails == (std::vector<std::string>{"Nobody@Team.Example", "dave@team.example", "empty@team.example"}));
  }
  SECTION("attachment size limits (C12)") {
    fx.ts.cfg.upload_body_limit = 1000;
    fx.ts.cfg.max_message_attachment_bytes = 1500;
    const auto big = fx.upload("big.bin", "application/octet-stream", false, 1001);
    DraftInput in;
    in.to = std::vector<Address>{kBob};
    in.attachment_ids = std::vector<int64_t>{big.id};
    const ApiError e1 = api_error([&] { fx.send(fx.create(in)); });
    CHECK(e1.status == 413);
    CHECK(e1.code == "message_too_large");
    const auto a = fx.upload("a.bin", "application/octet-stream", false, 800);
    const auto b = fx.upload("b.bin", "application/octet-stream", false, 800);
    in.attachment_ids = std::vector<int64_t>{a.id, b.id};
    const ApiError e2 = api_error([&] { fx.send(fx.create(in)); });
    CHECK(e2.code == "message_too_large");
    // (a is attached to the draft whose send failed above: the draft stays a draft.)
    const auto ok = fx.upload("ok.bin", "application/octet-stream", false, 800, 0, "ok-bytes");
    in.attachment_ids = std::vector<int64_t>{ok.id};
    const SendResult r = fx.send(fx.create(in));
    CHECK(fx.outbound(r.outbound_id).total_bytes == 800);
  }
  SECTION("schedule window [now + 60 s, now + 30 d]") {
    const Draft d = fx.simple_draft({kBob});
    for (int64_t at : {azm::now_ms() + 30'000, azm::now_ms() + 31LL * 24 * kHour, int64_t{1}}) {
      const ApiError e = api_error([&] { fx.send(d, at); });
      CHECK(e.status == 422);
      CHECK(e.code == "invalid_schedule");
    }
    CHECK_NOTHROW(fx.send(d, azm::now_ms() + 2 * 60'000));
  }
  SECTION("send-as is enforced at send time too") {
    DraftInput in;
    in.to = std::vector<Address>{kBob};
    in.from_address_id = fx.support;
    const Draft d = fx.create(in);
    fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE alias_members SET can_send_as = 0 WHERE user_id = ?", fx.alice); });
    const ApiError e = api_error([&] { fx.send(d); });
    CHECK(e.status == 403);
    CHECK(e.code == "send_as_forbidden");
  }
  SECTION("version conflict carries the current draft (6-arg) or nothing (5-arg)") {
    const Draft d = fx.simple_draft({kBob});
    SendOptions o;
    o.version = d.version + 7;
    const ApiError e = api_error([&] { fx.send_opts(d.id, o); });
    CHECK(e.code == "version_conflict");
    CHECK(e.details.at("current").as_object().at("id").as_int64() == d.id);
    const ApiError e5 = api_error([&] {
      fx.ts.db.write([&](db::Tx& tx) { queue_send(tx, fx.ts.cfg, fx.alice, d.id, o); });
    });
    CHECK(e5.code == "version_conflict");
    CHECK(e5.details.empty());
    CHECK(fx.scalar("SELECT COUNT(*) FROM outbound") == 0);  // a failed send queues nothing
    CHECK(fx.draft(d.id).has_value());                        // and leaves the draft a draft
  }
}

TEST_CASE("queue_send freeze: payload, headers, tags, signature above the quote", "[outbound][send][freeze]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE user_settings SET signature_enabled = 1, signature_html = '<p>张三 | 运营部</p>', "
           "undo_send_seconds = 5 WHERE user_id = ?", fx.alice);
  });
  // Parent: an inbound mail whose References chain is stored in order in inbound_emails.meta_json.
  const int64_t inbound_id = fx.ts.db.write([&](db::Tx& tx) {
    tx.run("INSERT INTO inbound_emails(resend_id, state, source, meta_json, created_at, updated_at) "
           "VALUES('rcv-1', 'delivered', 'webhook', ?, 1, 1)",
           std::string(R"({"references":["r1@x","r2@x"],"in_reply_to":"r2@x"})"));
    return tx.last_insert_id();
  });
  const auto parent = inbound_parent(fx, fx.alice, "p@customer.example", {"r1@x", "r2@x"}, "alice@team.example",
                                     inbound_id);
  const auto inl = fx.upload("i.png", "image/png", true);
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(parent.message_id);
  in.to = std::vector<Address>{{"王五 (采购)", "wang@customer.example"}};
  in.cc = std::vector<Address>{kBob, {"", "WANG@customer.example"}};  // duplicate of To is dropped
  in.bcc = std::vector<Address>{kCarol};
  in.html = "<p>好的</p><img data-att-id=\"" + std::to_string(inl.id) + "\" src=\"" + *inl.view_url + "\">";
  in.quoted_html = std::optional<std::string>("<blockquote>原邮件 <a href=\"" + fx.ts.urls.base_url() +
                                              "/api/files/999?d=a&sig=x\">链接</a></blockquote>");
  const Draft d = fx.create(in);
  const int64_t before = azm::now_ms();
  const SendResult r = fx.send(d);
  CHECK(r.message_id == d.id);
  CHECK(r.thread_id == parent.thread_id);
  CHECK(r.undo_ms == 5000);
  CHECK_FALSE(r.scheduled_at);

  const OutboundRow row = fx.outbound(r.outbound_id);
  CHECK(row.status == OutboundStatus::Queued);
  CHECK(row.send_after >= before + 5000);
  CHECK_FALSE(row.scheduled_at);
  CHECK_FALSE(row.scheduled_via);
  CHECK(row.uuid.size() == 36);
  CHECK(row.from_address_id == fx.alice_addr);
  CHECK(row.sender_user_id == fx.alice);

  const OutboundSendPlan p = fx.plan(r.outbound_id);
  CHECK(p.uuid == row.uuid);
  CHECK(p.from == "Alice <alice@team.example>");
  CHECK(p.to == std::vector<std::string>{"\"王五 (采购)\" <wang@customer.example>"});
  CHECK(p.cc == std::vector<std::string>{"Bob <bob@team.example>"});
  CHECK(p.bcc == std::vector<std::string>{"Carol <carol@team.example>"});
  CHECK(p.subject == "Re: 咨询");
  CHECK(header(p, "X-AzMail-Ref") == row.uuid);
  CHECK(header(p, "In-Reply-To") == "<p@customer.example>");
  CHECK(header(p, "References") == "<r1@x> <r2@x> <p@customer.example>");
  CHECK(p.tags == (std::vector<std::pair<std::string, std::string>>{{"azmail_outbound", row.uuid}}));
  CHECK_FALSE(p.parent_message_id_missing);
  // body, then the signature, then the quote; cid kept; data-att-id and our file URLs gone.
  const auto body_at = p.html.find("好的"), sig_at = p.html.find("张三 | 运营部"), quote_at = p.html.find("原邮件");
  REQUIRE(body_at != std::string::npos);
  REQUIRE(sig_at != std::string::npos);
  REQUIRE(quote_at != std::string::npos);
  CHECK(body_at < sig_at);
  CHECK(sig_at < quote_at);
  CHECK(p.html.find("cid:" + *inl.content_id) != std::string::npos);
  CHECK(p.html.find("data-att-id") == std::string::npos);
  CHECK(p.html.find("/api/files/") == std::string::npos);
  CHECK(p.html.find("font-family:") != std::string::npos);  // inline-styled wrapper
  CHECK(p.html.find("gmail_quote") != std::string::npos);
  CHECK(p.text.find("好的") != std::string::npos);
  CHECK(p.text.find("<p>") == std::string::npos);
  REQUIRE(p.attachments.size() == 1);
  CHECK(p.attachments[0].attachment_id == inl.id);
  CHECK(p.attachments[0].content_id == inl.content_id);
  CHECK(p.attachments[0].storage == "local");

  // The message is now the sender's sent copy.
  const auto v = fx.message(r.message_id).value();
  CHECK_FALSE(v.is_draft);
  CHECK(v.direction == Direction::Out);
  REQUIRE(v.outbound);
  CHECK(v.outbound->status == OutboundStatus::Queued);
  CHECK(v.cc.size() == 1);  // duplicates dropped in the stored copy as well
  CHECK(v.bcc == std::vector<Address>{kCarol});  // the sender keeps BCC (C2)
  REQUIRE(v.html);
  CHECK(v.html->find("/api/files/" + std::to_string(inl.id) + "?d=i") != std::string::npos);
  CHECK(fx.text_of("SELECT in_reply_to FROM messages WHERE id = ?", r.message_id) == "p@customer.example");
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM message_refs WHERE message_id = ?", r.message_id) == 3);
  CHECK(fx.folder_ids(Folder::Sent) == std::vector<int64_t>{parent.thread_id});
  CHECK(fx.folder_ids(Folder::Drafts).empty());
  // The send job (B1/B5/A6 contract).
  auto s = fx.ts.db.read([&](db::Conn& c) {
    auto q = c.prepare("SELECT kind, lane, priority, run_at, max_attempts, dedupe_key, payload FROM jobs WHERE id = ?");
    q.bind_all(*row.job_id);
    REQUIRE(q.step());
    CHECK(q.text(0) == "outbound.send");
    CHECK(q.text(1) == "outbound");
    CHECK(q.i64(2) == jobs::kPriorityHigh);
    CHECK(q.i64(3) == row.send_after);
    CHECK(q.i64(4) == jobs::kOutboundSendMaxAttempts);
    CHECK(q.text(5) == "out:send:" + std::to_string(r.outbound_id));
    return boost::json::parse(q.text(6)).as_object().at("outbound_id").as_int64();
  });
  CHECK(s == r.outbound_id);
  // Contacts: +1 per external recipient; team addresses are not stored.
  CHECK(fx.ts.db.read([&](db::Conn& c) {
          return c.scalar<double>("SELECT score FROM contacts WHERE owner_id = ? AND email = ?", fx.alice,
                                  std::string("wang@customer.example"));
        }) == 1.0);
  CHECK(fx.scalar("SELECT COUNT(*) FROM contacts WHERE email LIKE '%team.example'") == 0);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.queued'",
                     r.outbound_id) == 1);
}

TEST_CASE("queue_send: final draft fields saved atomically; unreferenced quote images dropped", "[outbound][send]") {
  SendFx fx;
  Msg m;
  m.owner = fx.alice;
  m.from = kCustomer;
  m.to = {kAlice};
  m.subject = "图片";
  m.html = "<img src=\"cid:pic1\">";
  m.message_id = "img@customer.example";
  Att pic;
  pic.filename = "pic.png";
  pic.content_type = "image/png";
  pic.content_id = "pic1";
  pic.is_inline = true;
  m.atts = {pic};
  const auto parent = fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_message(tx, m); });
  const Draft d = reply_to(fx, parent.message_id);
  REQUIRE(d.attachments.size() == 1);  // copied quote image

  // The user removed the quote; the final fields come with the send.
  SendOptions o;
  o.version = d.version;
  DraftInput fin;
  fin.html = "<p>最终内容</p>";
  fin.quoted_html = std::optional<std::string>();
  o.draft = fin;
  const SendResult r = fx.send_opts(d.id, o);
  const auto p = fx.plan(r.outbound_id);
  CHECK(p.html.find("最终内容") != std::string::npos);
  CHECK(p.attachments.empty());  // not referenced → not sent
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM attachments WHERE message_id = ?", d.id) == 0);

  // Forwards keep every attachment even when the body does not reference it.
  DraftInput fwd;
  fwd.mode = DraftMode::Forward;
  fwd.parent_message_id = std::optional<int64_t>(parent.message_id);
  fwd.to = std::vector<Address>{kExternal};
  const Draft f = fx.create(fwd);
  const auto fp = fx.plan(fx.send(f).outbound_id);
  CHECK(fp.attachments.size() == 1);
  CHECK(fp.subject == "Fwd: 图片");
  CHECK(header(fp, "In-Reply-To") == "<img@customer.example>");
}

TEST_CASE("signature: disabled, already in the body, image-only", "[outbound][send][freeze]") {
  SendFx fx;
  auto set_sig = [&](bool enabled, std::string html) {
    fx.ts.db.write([&](db::Tx& tx) {
      tx.run("UPDATE user_settings SET signature_enabled = ?, signature_html = ? WHERE user_id = ?", enabled, html,
             fx.alice);
    });
  };
  auto count = [](const std::string& h, const std::string& needle) {
    std::size_t n = 0;
    for (auto p = h.find(needle); p != std::string::npos; p = h.find(needle, p + 1)) ++n;
    return n;
  };
  set_sig(false, "<p>签名A</p>");
  CHECK(fx.plan(fx.send(fx.simple_draft({kBob})).outbound_id).html.find("签名A") == std::string::npos);
  set_sig(true, "<p>签名A</p>");
  const auto p1 = fx.plan(fx.send(fx.simple_draft({kBob}, "s", "<p>正文</p><p>签名A</p>")).outbound_id);
  CHECK(count(p1.html, "签名A") == 1);  // inserted by the editor already: not duplicated
  set_sig(true, "<img src=\"https://cdn.example/sig.png\">");
  const auto p2 = fx.plan(fx.send(fx.simple_draft({kBob})).outbound_id);
  CHECK(p2.html.find("https://cdn.example/sig.png") != std::string::npos);
  CHECK(p2.html.find("azm-signature") != std::string::npos);
}

TEST_CASE("scheduling decision: resend vs local (B5)", "[outbound][send][schedule]") {
  SendFx fx;
  const int64_t at = azm::now_ms() + 2 * kHour;
  SECTION("no attachments → resend; the job POSTs right away with scheduled_at") {
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    CHECK(r.scheduled_at == at);
    CHECK(r.undo_ms == 0);
    const auto row = fx.outbound(r.outbound_id);
    CHECK(row.scheduled_via == ScheduledVia::Resend);
    CHECK(row.scheduled_at == at);
    CHECK(fx.scalar_of("SELECT run_at FROM jobs WHERE id = ?", *row.job_id) == row.send_after);
    CHECK(fx.folder_ids(Folder::Scheduled) == std::vector<int64_t>{r.thread_id});
    CHECK(fx.folder_ids(Folder::Sent).empty());
    CHECK(fx.folder(Folder::Scheduled).items[0].scheduled_at == at);
    CHECK(fx.plan(r.outbound_id).scheduled_at == at);
  }
  SECTION("attachments → local; the job runs at scheduled_at") {
    const auto a = fx.upload("a.pdf", "application/pdf", false);
    DraftInput in;
    in.to = std::vector<Address>{kBob};
    in.attachment_ids = std::vector<int64_t>{a.id};
    const SendResult r = fx.send(fx.create(in), at);
    const auto row = fx.outbound(r.outbound_id);
    CHECK(row.scheduled_via == ScheduledVia::Local);
    CHECK(fx.scalar_of("SELECT run_at FROM jobs WHERE id = ?", *row.job_id) == at);
  }
  SECTION("AZMAIL_SCHEDULE_MODE=local") {
    fx.ts.cfg.schedule_mode = ScheduleMode::Local;
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    CHECK(fx.outbound(r.outbound_id).scheduled_via == ScheduledVia::Local);
  }
}

TEST_CASE("alias send with share_sent: shared copies for the other members (C3)", "[outbound][send][alias]") {
  SendFx fx;
  // A customer mail to support@ delivered to alice and bob (same inbound email).
  const int64_t inbound_id =
      fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_inbound_email(tx, "rcv-support", std::nullopt); });
  const auto a_parent = inbound_parent(fx, fx.alice, "ask@customer.example", {}, "support@team.example", inbound_id);
  const auto b_parent = inbound_parent(fx, fx.bob, "ask@customer.example", {}, "support@team.example", inbound_id);
  const auto att = fx.upload("报价.pdf", "application/pdf", false);
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(a_parent.message_id);
  in.html = "<p>您好，报价见附件</p>";
  in.bcc = std::vector<Address>{kCarol};
  in.attachment_ids = std::vector<int64_t>{att.id};
  const Draft d = fx.create(in);
  REQUIRE(d.from_address_id == fx.support);
  fx.ts.notifier.clear();
  const SendResult r = fx.send(d);

  // bob's shared copy: his own thread (with his copy of the customer's mail), no BCC, sent_by alice.
  const int64_t bob_copy = fx.scalar_of(
      "SELECT id FROM messages WHERE owner_id = ? AND outbound_id = ? AND is_shared_copy = 1", fx.bob, r.outbound_id);
  REQUIRE(bob_copy > 0);
  const auto v = fx.message(bob_copy, fx.bob).value();
  CHECK(v.thread_id == b_parent.thread_id);
  CHECK(v.direction == Direction::Out);
  CHECK(v.from == kSupport);
  REQUIRE(v.sent_by);
  CHECK(v.sent_by->email == "alice@team.example");
  CHECK(v.bcc.empty());
  CHECK(v.to == std::vector<Address>{kCustomer});
  REQUIRE(v.attachments.size() == 1);
  CHECK(v.attachments[0].filename == "报价.pdf");
  CHECK(v.attachments[0].id != att.id);  // bob's own attachment row (find_attachment is owner-scoped)
  REQUIRE(v.outbound);
  CHECK(v.outbound->id == r.outbound_id);
  CHECK(fx.folder_ids(Folder::Sent, fx.bob) == std::vector<int64_t>{b_parent.thread_id});
  CHECK(fx.events_of(ws::events::kThreadsChanged, fx.bob).size() == 1);
  // BCC is not searchable in the shared copy.
  CHECK(fx.ts.db.read([&](db::Conn& c) {
          return c.scalar<std::string>("SELECT to_text FROM message_fts WHERE rowid = ?", bob_copy).value_or("");
        }).find("carol") == std::string::npos);
  CHECK(fx.ts.db.read([&](db::Conn& c) {
          return c.scalar<std::string>("SELECT to_text FROM message_fts WHERE rowid = ?", r.message_id).value_or("");
        }).find("carol") != std::string::npos);
  // carol (not a member) gets nothing.
  CHECK(fx.message_ids(fx.carol).empty());

  // One outbound row, N copies: a status change reaches both owners.
  fx.accept(r.outbound_id, "re_shared");
  fx.ts.notifier.clear();
  fx.event("re_shared", "email.delivered", "svix-d1");
  CHECK(fx.events_of(ws::events::kOutboundStatus, fx.alice).size() == 1);
  const auto bob_status = fx.events_of(ws::events::kOutboundStatus, fx.bob);
  REQUIRE(bob_status.size() == 1);
  CHECK(bob_status[0].data.at("message_id").as_int64() == bob_copy);
  CHECK(bob_status[0].data.at("status").as_string() == "delivered");
  CHECK(fx.message(bob_copy, fx.bob)->outbound->status == OutboundStatus::Delivered);
}

TEST_CASE("alias shared copies: share_sent=0, disabled members, undo removes them", "[outbound][send][alias]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE user_settings SET undo_send_seconds = 5 WHERE user_id = ?", fx.alice);
    tx.run("INSERT INTO alias_members(alias_id, user_id, can_send_as) VALUES(?, ?, 0)", fx.support, fx.carol);
    tx.run("UPDATE users SET disabled = 1 WHERE id = ?", fx.carol);
  });
  DraftInput in;
  in.to = std::vector<Address>{kCustomer};
  in.from_address_id = fx.support;
  in.html = "<p>通知</p>";
  const SendResult r = fx.send(fx.create(in));
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM messages WHERE outbound_id = ? AND is_shared_copy = 1", r.outbound_id) ==
        1);  // bob only (carol disabled)
  const int64_t bob_thread =
      fx.scalar_of("SELECT thread_id FROM messages WHERE owner_id = ? AND outbound_id = ?", fx.bob, r.outbound_id);
  fx.ts.notifier.clear();
  fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.alice, r.message_id); });
  CHECK(fx.message_ids(fx.bob).empty());
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM threads WHERE id = ?", bob_thread) == 0);
  CHECK(fx.events_of(ws::events::kThreadsChanged, fx.bob).size() == 1);

  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE addresses SET share_sent = 0 WHERE id = ?", fx.support); });
  const SendResult r2 = fx.send(fx.create(in));
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM messages WHERE outbound_id = ?", r2.outbound_id) == 1);
}

TEST_CASE("load_send_plan resolves a parent Message-ID captured after the freeze (B2)", "[outbound][plan]") {
  SendFx fx;
  // alice sends a first mail; bob's... no: alice replies to her OWN sent mail before its id is known.
  const SendResult first = fx.send(fx.simple_draft({kExternal}, "进度"));
  const Draft follow_up = reply_to(fx, first.message_id);
  CHECK(follow_up.to == std::vector<Address>{kExternal});  // reply to own sent mail → its recipients
  const SendResult second = fx.send(follow_up);
  CHECK(fx.outbound(second.outbound_id).parent_outbound_id == first.outbound_id);

  OutboundSendPlan p = fx.plan(second.outbound_id);
  CHECK(p.parent_message_id_missing);
  CHECK(p.parent_outbound_id == first.outbound_id);
  CHECK_FALSE(p.parent_resend_id);  // the first send was not accepted yet
  CHECK(header(p, "In-Reply-To").empty());

  fx.accept(first.outbound_id, "re_first");
  p = fx.plan(second.outbound_id);
  CHECK(p.parent_resend_id == "re_first");

  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, first.outbound_id, "<first@resend.dev>"); });
  p = fx.plan(second.outbound_id);
  CHECK_FALSE(p.parent_message_id_missing);
  CHECK(header(p, "In-Reply-To") == "<first@resend.dev>");
  CHECK(header(p, "References") == "<first@resend.dev>");

  // A reply to the reply chains both ids (parent's References from its frozen payload).
  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, second.outbound_id, "second@resend.dev"); });
  const SendResult third = fx.send(reply_to(fx, second.message_id));
  CHECK(header(fx.plan(third.outbound_id), "References") == "<first@resend.dev> <second@resend.dev>");
  CHECK_THROWS_AS(fx.plan(987654), std::out_of_range);
}

TEST_CASE("References keep the last 20 ids, in header order", "[outbound][plan]") {
  SendFx fx;
  std::vector<std::string> refs;
  for (int i = 0; i < 30; ++i) refs.push_back("<r" + std::to_string(i) + "@x>");
  // Delivered for real: the ordered chain is kept in inbound_emails.meta_json (message_refs is a set).
  auto e = SendFx::inbound("rcv-chain", kCustomer, {kAlice}, {"alice@team.example"}, "Re: 长链", "last@x");
  e.references = refs;
  e.in_reply_to = "r29@x";
  const auto res = fx.deliver(e);
  REQUIRE(res.copies.size() == 1);
  const auto p = fx.plan(fx.send(reply_to(fx, res.copies[0].message_id)).outbound_id);
  const std::string h = header(p, "References");
  CHECK(h.find("<r10@x>") == std::string::npos);
  CHECK(h.rfind("<r11@x>", 0) == 0);
  CHECK(h.size() > 7);
  CHECK(h.substr(h.size() - 8) == "<last@x>");
  std::size_t n = std::count(h.begin(), h.end(), '<');
  CHECK(n == 20);
}

TEST_CASE("job transitions: sending, accepted, failed, retry note, local schedule switch", "[outbound][jobs]") {
  SendFx fx;
  const SendResult r = fx.send(fx.simple_draft({kBob}));
  fx.ts.notifier.clear();
  CHECK(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r.outbound_id); }));
  CHECK(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r.outbound_id); }));  // retries re-enter
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.sending'",
                     r.outbound_id) == 1);
  CHECK(fx.events_of(ws::events::kOutboundStatus, fx.alice).size() == 1);
  fx.ts.db.write([&](db::Tx& tx) { note_send_retry(tx, r.outbound_id, "internal_server_error", "发送重试中"); });
  CHECK(fx.outbound(r.outbound_id).status_detail == "发送重试中");
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Sending);
  fx.ts.db.write([&](db::Tx& tx) { mark_accepted(tx, r.outbound_id, "re_123", false); });
  auto row = fx.outbound(r.outbound_id);
  CHECK(row.status == OutboundStatus::Accepted);
  CHECK(row.resend_id == "re_123");
  CHECK(row.accepted_at);
  CHECK_FALSE(row.status_detail);  // the retry note is cleared on success
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r.outbound_id); }));
  CHECK(fx.ts.db.read([&](db::Conn& c) { return find_outbound(c, std::string_view("re_123"), std::nullopt); }) ==
        r.outbound_id);
  CHECK(fx.ts.db.read([&](db::Conn& c) { return find_outbound(c, std::nullopt, std::string_view(row.uuid)); }) ==
        r.outbound_id);
  CHECK_FALSE(fx.ts.db.read([&](db::Conn& c) { return find_outbound(c, std::string_view("nope"), std::nullopt); }));
  CHECK(fx.message(r.message_id)->outbound->sent_at == row.accepted_at);

  // A failure (quota) is terminal and visible.
  const SendResult f = fx.send(fx.simple_draft({kBob}));
  fx.ts.db.write([&](db::Tx& tx) {
    mark_sending(tx, f.outbound_id);
    mark_failed(tx, f.outbound_id, "daily_quota_exceeded", "发送配额已用完");
  });
  row = fx.outbound(f.outbound_id);
  CHECK(row.status == OutboundStatus::Failed);
  CHECK(row.error_name == "daily_quota_exceeded");
  CHECK(row.status_detail == "发送配额已用完");
  CHECK(fx.message(f.message_id)->outbound->status_detail == "发送配额已用完");
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.failed'",
                     f.outbound_id) == 1);

  // Resend refused the scheduling: local scheduling at the same time.
  const int64_t at = azm::now_ms() + kHour;
  const SendResult s = fx.send(fx.simple_draft({kBob}), at);
  fx.ts.db.write([&](db::Tx& tx) { mark_sending(tx, s.outbound_id); });
  CHECK(fx.ts.db.write([&](db::Tx& tx) { return switch_to_local_schedule(tx, s.outbound_id); }) == at);
  row = fx.outbound(s.outbound_id);
  CHECK(row.scheduled_via == ScheduledVia::Local);
  CHECK(row.status == OutboundStatus::Queued);
  CHECK_THROWS_AS(fx.ts.db.write([&](db::Tx& tx) { return switch_to_local_schedule(tx, r.outbound_id); }),
                  std::logic_error);
  // Missing rows are harmless for the job API.
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, 999999); }));
  CHECK_NOTHROW(fx.ts.db.write([&](db::Tx& tx) { mark_accepted(tx, 999999, "x", false); }));
}

TEST_CASE("cancel-schedule: local, remote, in-flight, already sent", "[outbound][schedule]") {
  SendFx fx;
  const int64_t at = azm::now_ms() + 2 * kHour;
  auto begin = [&](int64_t mid, int64_t owner = 0) {
    return fx.ts.db.write([&](db::Tx& tx) { return begin_cancel_schedule(tx, fx.ts.urls, owner ? owner : fx.alice, mid); });
  };
  SECTION("queued (POST not made yet) is canceled locally → draft") {
    const SendResult r = fx.send(fx.simple_draft({kBob}, "定时"), at);
    const CancelPlan p = begin(r.message_id);
    CHECK_FALSE(p.remote);
    REQUIRE(p.draft);
    CHECK(p.draft->id == r.message_id);
    CHECK(p.draft->subject == "定时");
    CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
    CHECK(fx.folder_ids(Folder::Scheduled).empty());
    CHECK(fx.folder_ids(Folder::Drafts) == std::vector<int64_t>{r.thread_id});
  }
  SECTION("scheduled at Resend → remote plan, then finish → draft") {
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    fx.accept(r.outbound_id, "re_sched", /*scheduled=*/true);
    CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Scheduled);
    CHECK(fx.folder_ids(Folder::Scheduled) == std::vector<int64_t>{r.thread_id});
    const CancelPlan p = begin(r.message_id);
    CHECK(p.remote);
    CHECK(p.resend_id == "re_sched");
    CHECK(p.outbound_id == r.outbound_id);
    CHECK_FALSE(p.draft);
    CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Scheduled);  // unchanged until finish
    const Draft d = fx.ts.db.write(
        [&](db::Tx& tx) { return finish_cancel_schedule(tx, fx.ts.urls, fx.alice, r.outbound_id); });
    CHECK(d.id == r.message_id);
    CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
    CHECK(fx.folder_ids(Folder::Scheduled).empty());
    CHECK(api_error([&] {
            fx.ts.db.write([&](db::Tx& tx) { finish_cancel_schedule(tx, fx.ts.urls, fx.bob, r.outbound_id); });
          }).status == 404);
  }
  SECTION("scheduling POST in flight → invalid_state; send under way / done → already_sent") {
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    fx.ts.db.write([&](db::Tx& tx) { mark_sending(tx, r.outbound_id); });
    const ApiError e = api_error([&] { begin(r.message_id); });
    CHECK(e.code == "invalid_state");
    CHECK(e.message == "正在提交定时发送，请稍后重试");
    fx.ts.db.write([&](db::Tx& tx) { mark_accepted(tx, r.outbound_id, "re_x", true); });
    fx.event("re_x", "email.sent", "svix-s");
    CHECK(api_error([&] { begin(r.message_id); }).code == "already_sent");
  }
  SECTION("local scheduling that already went out → already_sent; not scheduled → invalid_state; IDOR") {
    fx.ts.cfg.schedule_mode = ScheduleMode::Local;
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    fx.accept(r.outbound_id, "re_local", false);  // the local job sent it at its time
    CHECK(api_error([&] { begin(r.message_id); }).code == "already_sent");
    const SendResult now_send = fx.send(fx.simple_draft({kBob}));
    CHECK(api_error([&] { begin(now_send.message_id); }).code == "invalid_state");
    CHECK(api_error([&] { begin(r.message_id, fx.bob); }).status == 404);
    CHECK(api_error([&] { begin(424242); }).status == 404);
  }
}

TEST_CASE("reschedule: local applies immediately, remote returns a plan, finish moves fetch_meta", "[outbound][schedule]") {
  SendFx fx;
  const int64_t now = azm::now_ms();
  const int64_t at = now + 2 * kHour, later = now + 5 * kHour;
  auto begin = [&](int64_t mid, int64_t when, int64_t owner = 0) {
    return fx.ts.db.write(
        [&](db::Tx& tx) { return begin_reschedule(tx, fx.ts.cfg, owner ? owner : fx.alice, mid, when, azm::now_ms()); });
  };
  SECTION("local") {
    fx.ts.cfg.schedule_mode = ScheduleMode::Local;
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    fx.ts.notifier.clear();
    const ReschedulePlan p = begin(r.message_id, later);
    CHECK_FALSE(p.remote);
    CHECK(p.scheduled_at == later);
    const auto row = fx.outbound(r.outbound_id);
    CHECK(row.scheduled_at == later);
    CHECK(fx.scalar_of("SELECT run_at FROM jobs WHERE id = ?", *row.job_id) == later);
    CHECK_FALSE(fx.events_of(ws::events::kThreadsChanged, fx.alice).empty());
    CHECK(api_error([&] { begin(r.message_id, now + 1000); }).code == "invalid_schedule");
    CHECK(api_error([&] { begin(r.message_id, later, fx.bob); }).status == 404);
  }
  SECTION("remote") {
    const SendResult r = fx.send(fx.simple_draft({kBob}), at);
    fx.accept(r.outbound_id, "re_r", true);
    const ReschedulePlan p = begin(r.message_id, later);
    CHECK(p.remote);
    CHECK(p.resend_id == "re_r");
    CHECK(fx.outbound(r.outbound_id).scheduled_at == at);  // not before Resend agreed
    // fetch_meta enqueued by the send job for scheduled_at + 60 s
    const int64_t job = fx.ts.db.write([&](db::Tx& tx) {
      jobs::EnqueueOpts o;
      o.run_at_ms = at + 60'000;
      o.dedupe_key = jobs::dedupe_fetch_meta(r.outbound_id);
      return jobs::enqueue(tx, jobs::kinds::kOutboundFetchMeta, {{"outbound_id", r.outbound_id}}, o);
    });
    fx.ts.db.write([&](db::Tx& tx) { finish_reschedule(tx, fx.alice, r.outbound_id, later); });
    CHECK(fx.outbound(r.outbound_id).scheduled_at == later);
    CHECK(fx.scalar_of("SELECT run_at FROM jobs WHERE id = ?", job) == later + 60'000);
    CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.rescheduled'",
                       r.outbound_id) == 1);
    CHECK(fx.folder(Folder::Scheduled).items.at(0).scheduled_at == later);
    CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { finish_reschedule(tx, fx.bob, r.outbound_id, later); }); })
              .status == 404);
  }
}

TEST_CASE("retry of a failed send: new outbound row and uuid, copies re-pointed", "[outbound][retry]") {
  SendFx fx;
  DraftInput in;
  in.to = std::vector<Address>{kCustomer};
  in.from_address_id = fx.support;  // with a shared copy for bob
  const SendResult r = fx.send(fx.create(in));
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { retry_failed_send(tx, fx.alice, r.message_id, 0); }); })
            .code == "invalid_state");
  fx.ts.db.write([&](db::Tx& tx) {
    mark_sending(tx, r.outbound_id);
    mark_failed(tx, r.outbound_id, "validation_error", "发送失败");
  });
  const SendResult again =
      fx.ts.db.write([&](db::Tx& tx) { return retry_failed_send(tx, fx.alice, r.message_id, azm::now_ms()); });
  CHECK(again.outbound_id != r.outbound_id);
  CHECK(again.message_id == r.message_id);
  CHECK(again.status == OutboundStatus::Queued);
  const auto old_row = fx.outbound(r.outbound_id), new_row = fx.outbound(again.outbound_id);
  CHECK(old_row.status == OutboundStatus::Failed);  // history
  CHECK(new_row.uuid != old_row.uuid);
  CHECK(new_row.status == OutboundStatus::Queued);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM messages WHERE outbound_id = ?", again.outbound_id) == 2);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM messages WHERE outbound_id = ?", r.outbound_id) == 0);
  CHECK(fx.plan(again.outbound_id).html == fx.plan(r.outbound_id).html);
  CHECK(header(fx.plan(again.outbound_id), "X-AzMail-Ref") == new_row.uuid);
  CHECK(fx.text_of("SELECT dedupe_key FROM jobs WHERE id = ?", *new_row.job_id) ==
        "out:send:" + std::to_string(again.outbound_id));
  // bob cannot retry alice's send through his shared copy.
  const int64_t bob_copy = fx.scalar_of("SELECT id FROM messages WHERE owner_id = ?", fx.bob);
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { retry_failed_send(tx, fx.bob, bob_copy, 0); }); }).status ==
        404);

  // Admin retry of any failed outbound.
  fx.ts.db.write([&](db::Tx& tx) { mark_failed(tx, again.outbound_id, "x", "发送失败"); });
  const int64_t third = fx.ts.db.write([&](db::Tx& tx) { return admin_retry_outbound(tx, again.outbound_id, 0); });
  CHECK(third != again.outbound_id);
  CHECK(fx.outbound(third).status == OutboundStatus::Queued);
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { admin_retry_outbound(tx, third, 0); }); }).code ==
        "invalid_state");
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { admin_retry_outbound(tx, 777777, 0); }); }).status == 404);
}

TEST_CASE("set_outbound_message_id: propagation and late thread merge (reply arrived first, B2)", "[outbound][msgid]") {
  SendFx fx;
  DraftInput in;
  in.to = std::vector<Address>{kCustomer};
  in.from_address_id = fx.support;
  in.subject = "报价单";
  const SendResult r = fx.send(fx.create(in));
  fx.accept(r.outbound_id, "re_q");
  // The customer's auto-reply arrives before we know our Message-ID: it opens new threads.
  const auto early_alice = inbound_parent(fx, fx.alice, "auto@customer.example", {"ours@resend.dev"});
  const auto early_bob = inbound_parent(fx, fx.bob, "auto@customer.example", {"ours@resend.dev"});
  CHECK(early_alice.thread_id != r.thread_id);
  fx.ts.notifier.clear();
  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, r.outbound_id, "<ours@resend.dev>"); });
  CHECK(fx.outbound(r.outbound_id).message_id_header == "ours@resend.dev");
  CHECK(fx.scalar("SELECT COUNT(*) FROM messages WHERE message_id_header = 'ours@resend.dev'") == 2);  // both copies
  // Each owner's reply thread merged into the thread of their copy.
  CHECK(fx.ts.db.read([&](db::Conn& c) { return test::mailfx::thread_of(c, early_alice.message_id); }) == r.thread_id);
  const int64_t bob_thread =
      fx.scalar_of("SELECT thread_id FROM messages WHERE owner_id = ? AND outbound_id = ?", fx.bob, r.outbound_id);
  CHECK(fx.ts.db.read([&](db::Conn& c) { return test::mailfx::thread_of(c, early_bob.message_id); }) == bob_thread);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM threads WHERE id = ?", early_alice.thread_id) == 0);
  CHECK_FALSE(fx.events_of(ws::events::kThreadsChanged, fx.bob).empty());
  // Idempotent; a different id never overwrites.
  fx.ts.db.write([&](db::Tx& tx) {
    set_outbound_message_id(tx, r.outbound_id, "ours@resend.dev");
    set_outbound_message_id(tx, r.outbound_id, "other@resend.dev");
    set_outbound_message_id(tx, r.outbound_id, "  ");
  });
  CHECK(fx.outbound(r.outbound_id).message_id_header == "ours@resend.dev");
}

TEST_CASE("set_outbound_message_id folds an earlier loopback 'in' copy into the out copy (§H 27)", "[outbound][msgid][loopback]") {
  SendFx fx;
  const SendResult r = fx.send(fx.simple_draft({kAlice}, "给自己"));
  fx.accept(r.outbound_id, "re_self");
  // The loopback arrives before the id is known and without X-AzMail-Ref (stripped by a relay).
  auto e = SendFx::inbound("rcv-self", kAlice, {kAlice}, {"alice@team.example"}, "给自己", "self@resend.dev");
  const auto res = fx.deliver(e);
  REQUIRE(res.copies.size() == 1);
  CHECK_FALSE(res.copies[0].loopback_merged);
  const int64_t in_copy = res.copies[0].message_id;
  const int64_t label = fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_label(tx, fx.alice, "重要"); });
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("INSERT INTO message_labels(message_id, label_id) VALUES(?, ?)", in_copy, label);
    tx.run("UPDATE messages SET is_starred = 1 WHERE id = ?", in_copy);
  });
  const Draft reply_draft = reply_to(fx, in_copy);  // a draft replying to the loopback copy

  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, r.outbound_id, "self@resend.dev"); });
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM messages WHERE id = ?", in_copy) == 0);
  const auto v = fx.message(r.message_id).value();
  CHECK(v.in_inbox);
  CHECK(v.is_starred);
  CHECK(v.label_ids == std::vector<int64_t>{label});
  CHECK(v.is_read);  // the sender's own copy stays read
  CHECK(fx.draft(reply_draft.id)->parent_message_id == r.message_id);
  CHECK(fx.folder_ids(Folder::Inbox) == std::vector<int64_t>{r.thread_id});
  CHECK(fx.ts.db.read([&](db::Conn& c) { return test::mailfx::thread_of(c, reply_draft.id); }) == r.thread_id);
  CHECK(fx.folder(Folder::All).items.size() == 1);
}

TEST_CASE("outbound_to_reconcile picks recent non-terminal sends with a resend id", "[outbound][reconcile]") {
  SendFx fx;
  const SendResult a = fx.send(fx.simple_draft({kBob}));
  const SendResult b = fx.send(fx.simple_draft({kBob}));
  const SendResult c = fx.send(fx.simple_draft({kBob}));
  const SendResult q = fx.send(fx.simple_draft({kBob}));  // still queued: not polled
  fx.accept(a.outbound_id, "re_a");
  fx.accept(b.outbound_id, "re_b");
  fx.accept(c.outbound_id, "re_c");
  fx.event("re_b", "email.delivered", "k1", azm::now_ms() - 5000);  // delivered: nothing left to poll
  fx.event("re_c", "email.bounced", "k2");
  fx.event("re_a", "email.sent", "k3", azm::now_ms() - 10'000);
  const auto items = fx.ts.db.read([&](db::Conn& cn) { return outbound_to_reconcile(cn, azm::now_ms(), 7 * 24 * kHour, 50); });
  REQUIRE(items.size() == 1);  // a only (b delivered, c bounced, q has no resend id)
  CHECK(items[0].outbound_id == a.outbound_id);
  CHECK(items[0].resend_id == "re_a");
  CHECK(items[0].status == OutboundStatus::Sent);
  CHECK(fx.ts.db.read([&](db::Conn& cn) { return outbound_to_reconcile(cn, azm::now_ms() + 30 * 24 * kHour, 7 * 24 * kHour, 50); })
            .empty());
  (void)q;
}

TEST_CASE("queue_send edge cases: deleted alias identity, 5-arg overload, snippet, failing final fields",
          "[outbound][send]") {
  SendFx fx;
  SECTION("a draft whose alias was deleted falls back to the own mailbox") {
    DraftInput in;
    in.to = std::vector<Address>{kBob};
    in.from_address_id = fx.support;
    const Draft d = fx.create(in);
    fx.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM addresses WHERE id = ?", fx.support); });
    CHECK(fx.draft(d.id)->from_address_id == fx.alice_addr);
    const SendResult r = fx.ts.db.write([&](db::Tx& tx) {
      SendOptions o;
      o.version = d.version;
      return queue_send(tx, fx.ts.cfg, fx.alice, d.id, o);  // the 5-argument overload
    });
    CHECK(fx.plan(r.outbound_id).from == "Alice <alice@team.example>");
    CHECK(fx.outbound(r.outbound_id).from_address_id == fx.alice_addr);
  }
  SECTION("the sent snippet leaves the quote out") {
    DraftInput in;
    in.to = std::vector<Address>{kBob};
    in.html = "<p>收到，谢谢</p>";
    in.quoted_html =
        std::optional<std::string>("<p>在 2026年10月7日，王五 写道：</p><blockquote>很长的原文</blockquote>");
    const SendResult r = fx.send(fx.create(in));
    CHECK(fx.text_of("SELECT snippet FROM messages WHERE id = ?", r.message_id) == "收到，谢谢");
    CHECK(fx.plan(r.outbound_id).html.find("很长的原文") != std::string::npos);
  }
  SECTION("invalid final fields roll the whole send back") {
    const Draft d = fx.simple_draft({kBob});
    SendOptions o;
    o.version = d.version;
    DraftInput bad;
    bad.attachment_ids = std::vector<int64_t>{31337};
    o.draft = bad;
    CHECK(api_error([&] { fx.send_opts(d.id, o); }).code == "invalid_field");
    CHECK(fx.scalar("SELECT COUNT(*) FROM outbound") == 0);
    CHECK(fx.draft(d.id)->version == d.version);
  }
  SECTION("members cannot undo or cancel the sender's send; finishing twice is invalid") {
    DraftInput in;
    in.to = std::vector<Address>{kCustomer};
    in.from_address_id = fx.support;
    const SendResult r = fx.send(fx.create(in), azm::now_ms() + kHour);
    const int64_t bob_copy = fx.scalar_of("SELECT id FROM messages WHERE owner_id = ?", fx.bob);
    CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.bob, bob_copy); }); })
              .status == 404);
    CHECK(api_error([&] {
            fx.ts.db.write([&](db::Tx& tx) { begin_cancel_schedule(tx, fx.ts.urls, fx.bob, bob_copy); });
          }).status == 404);
    fx.accept(r.outbound_id, "re_twice", true);
    fx.ts.db.write([&](db::Tx& tx) { finish_cancel_schedule(tx, fx.ts.urls, fx.alice, r.outbound_id); });
    CHECK(fx.message_ids(fx.bob).empty());  // the shared copy went with the cancel
    CHECK(api_error([&] {
            fx.ts.db.write([&](db::Tx& tx) { finish_cancel_schedule(tx, fx.ts.urls, fx.alice, r.outbound_id); });
          }).code == "invalid_state");
  }
}
