// Owner: WP-B — regression tests for the be_mail review findings (R1–R12, SEC-1/3/4/5/8, RT-3,
// F4): each TEST_CASE names the finding it pins down.
#include "send_fixtures.hpp"

#include "api/handlers.hpp"
#include "core/strings.hpp"
#include "http/router.hpp"
#include "jobs/handlers.hpp"
#include "jobs/jobs.hpp"
#include "jobs/kinds.hpp"
#include "mail/eml.hpp"
#include "mail/html_text.hpp"
#include "mail/send_internal.hpp"
#include "resend/client.hpp"
#include "ws/events.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <variant>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::sendfx;
using namespace azm::mail::eml;
using test::mailfx::Att;
using test::mailfx::Msg;

namespace {

constexpr int64_t kHour = 3'600'000;
constexpr int64_t kDay = 24 * kHour;

bool json_ok(const std::string& s) {
  boost::system::error_code ec;
  (void)boost::json::parse(s, ec);
  return !ec;
}

template <class F>
double seconds_of(F&& f) {
  const auto t0 = std::chrono::steady_clock::now();
  f();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string repeat(std::string_view s, std::size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (std::size_t i = 0; i < n; ++i) out += s;
  return out;
}

std::size_t count_of(const std::string& h, const std::string& needle) {
  std::size_t n = 0;
  for (auto p = h.find(needle); p != std::string::npos; p = h.find(needle, p + 1)) ++n;
  return n;
}

// Scripted Resend double for the job and API paths.
struct FakeResend final : resend::Client {
  std::vector<resend::SendRequest> sends;
  std::deque<std::variant<std::string, resend::Error>> send_results;
  std::map<std::string, resend::SentEmail> emails;
  std::optional<resend::Error> cancel_error;
  std::vector<std::string> canceled;

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
  void cancel(std::string_view id) override {
    if (cancel_error) throw *cancel_error;
    canceled.emplace_back(id);
  }
};

jobs::Job send_job(int64_t outbound_id, int attempts) {
  jobs::Job j;
  j.id = 1;
  j.kind = std::string(jobs::kinds::kOutboundSend);
  j.payload = {{"outbound_id", outbound_id}};
  j.attempts = attempts;
  j.max_attempts = jobs::kOutboundSendMaxAttempts;
  j.lane = "outbound";
  return j;
}

std::vector<int64_t> actions(SendFx& fx, std::vector<int64_t> threads, ThreadAction a, int64_t owner = 0) {
  return fx.ts.db.write(
      [&](db::Tx& tx) { return apply_thread_action(tx, owner ? owner : fx.alice, threads, a, std::nullopt); });
}

const ThreadListItem* item_of(const ThreadPage& p, int64_t thread_id) {
  for (const auto& t : p.items)
    if (t.id == thread_id) return &t;
  return nullptr;
}

std::string status_of(SendFx& fx, int64_t outbound_id) {
  return fx.text_of("SELECT status FROM outbound WHERE id = ?", outbound_id);
}

}  // namespace

// =============================================================================================
// R1 / SEC-8 — Message-IDs and Content-IDs are header-safe and never break payload_json
// =============================================================================================

TEST_CASE("R1/SEC-8: sanitize_message_id and parse_msgid_list keep only header-safe ids", "[review][R1][SEC-8]") {
  CHECK(sanitize_message_id(" <abc@x.example> ") == "abc@x.example");
  CHECK(utf8_valid(sanitize_message_id("<caf\xE9@ext.example>")));
  CHECK_FALSE(sanitize_message_id("<caf\xE9@ext.example>").empty());
  CHECK(sanitize_message_id("<a\r\nBcc: x@y>").empty());
  CHECK(sanitize_message_id("a b@x").empty());
  CHECK(sanitize_message_id("a\"b@x").empty());
  CHECK(sanitize_message_id(std::string("a\0b@x", 5)).empty());
  CHECK(sanitize_message_id(std::string(999, 'a')).empty());

  const auto ids = parse_msgid_list("<caf\xE9@ext.example> <ok@x.example>\r\n <r\xFF\xFE@y>");
  REQUIRE(ids.size() == 3);
  for (const auto& id : ids) CHECK(utf8_valid(id));
  CHECK(ids[1] == "ok@x.example");
}

TEST_CASE("R1: an 8-bit inbound Message-ID still gives a sendable reply and a lossless undo", "[review][R1]") {
  SendFx fx;
  auto e = SendFx::inbound("rcv-8bit", kCustomer, {kAlice}, {"alice@team.example"}, "询价", "caf\xE9@ext.example");
  e.in_reply_to = "pr\xE9v@ext.example";
  e.references = {"r1\xFF@ext.example", "pr\xE9v@ext.example"};
  const auto r = fx.deliver(e);
  REQUIRE(r.copies.size() == 1);
  const int64_t parent = r.copies[0].message_id;
  CHECK(utf8_valid(fx.text_of("SELECT message_id_header FROM messages WHERE id = ?", parent)));
  CHECK(utf8_valid(fx.text_of("SELECT in_reply_to FROM messages WHERE id = ?", parent)));
  CHECK(json_ok(fx.text_of("SELECT meta_json FROM inbound_emails WHERE resend_id = 'rcv-8bit'")));
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM message_refs WHERE message_id = ?", parent) >= 2);

  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE user_settings SET undo_send_seconds = 30"); });
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(parent);
  in.html = "<p>报价如下</p>";
  const SendResult s = fx.send(fx.create(in));
  CHECK(json_ok(fx.text_of("SELECT payload_json FROM outbound WHERE id = ?", s.outbound_id)));
  const OutboundSendPlan p = fx.plan(s.outbound_id);
  CHECK(p.from == "Alice <alice@team.example>");
  REQUIRE(p.to.size() == 1);
  CHECK(p.html.find("报价如下") != std::string::npos);
  CHECK_FALSE(header(p, "In-Reply-To").empty());
  CHECK(utf8_valid(header(p, "References")));

  // Undo restores exactly what the user wrote.
  const Draft d = fx.ts.db.write([&](db::Tx& tx) { return undo_send(tx, fx.ts.urls, fx.alice, s.message_id); });
  CHECK(d.html.find("报价如下") != std::string::npos);
}

TEST_CASE("R1: an unreadable payload fails loudly and never empties the body", "[review][R1]") {
  SendFx fx;
  CHECK_THROWS_AS(detail::payload_from_json("{\"from\":"), detail::PayloadCorrupt);
  CHECK_THROWS_AS(detail::payload_from_json("[1,2]"), detail::PayloadCorrupt);
  // Rows frozen before the sanitizing writer may hold raw 8-bit bytes: still readable.
  const auto old = detail::payload_from_json("{\"from\":\"A <a@x>\",\"in_reply_to\":\"caf\xE9@x\",\"draft\":{\"html\":\"<p>x</p>\"}}");
  CHECK(old.from == "A <a@x>");
  REQUIRE(old.in_reply_to);
  CHECK(utf8_valid(*old.in_reply_to));
  CHECK(old.has_draft);

  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE user_settings SET undo_send_seconds = 30"); });
  const SendResult s = fx.send(fx.simple_draft({kBob}, "正文保留", "<p>不能丢的正文</p>"));
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE outbound SET payload_json = 'garbage{' WHERE id = ?", s.outbound_id); });
  try {
    (void)fx.plan(s.outbound_id);
    FAIL("load_send_plan must refuse a corrupt payload");
  } catch (const UnsendableOutbound& e) {
    CHECK(e.name == "payload_corrupt");
  }
  const Draft d = fx.ts.db.write([&](db::Tx& tx) { return undo_send(tx, fx.ts.urls, fx.alice, s.message_id); });
  CHECK(d.html.find("不能丢的正文") != std::string::npos);
}

TEST_CASE("SEC-8: an inbound Content-ID with CR/LF is never stored or forwarded", "[review][SEC-8]") {
  SendFx fx;
  auto e = SendFx::inbound("rcv-cid", kCustomer, {kAlice}, {"alice@team.example"}, "附件");
  InboundAttachment a;
  a.blob = BlobRef{crypto::sha256_hex("cid-bytes"), 9, "local"};
  a.filename = "a.png";
  a.content_type = "image/png";
  a.disposition = "inline";
  a.content_id = "pic1\r\nBcc: victim@x.example";
  e.attachments = {a};
  const auto r = fx.deliver(e);
  REQUIRE(r.copies.size() == 1);
  CHECK(fx.text_of("SELECT content_id FROM attachments WHERE message_id = ?", r.copies[0].message_id) == "<null>");
}

// =============================================================================================
// R2 — removing the sender's copy of a pending send
// =============================================================================================

TEST_CASE("R2: trash / delete forever of a pending send cancels it or is refused", "[review][R2]") {
  SendFx fx;
  SECTION("trash of a local schedule cancels it in the same transaction (copy → trashed draft)") {
    const auto att = fx.upload("合同.pdf", "application/pdf", false);
    DraftInput in;
    in.to = std::vector<Address>{kExternal};
    in.subject = "合同";
    in.html = "<p>请查收</p>";
    in.attachment_ids = std::vector<int64_t>{att.id};
    const SendResult s = fx.send(fx.create(in), azm::now_ms() + 24 * kHour);
    REQUIRE(fx.outbound(s.outbound_id).scheduled_via == ScheduledVia::Local);
    const int64_t job = *fx.outbound(s.outbound_id).job_id;
    actions(fx, {s.thread_id}, ThreadAction::Trash);
    CHECK(status_of(fx, s.outbound_id) == "canceled");
    CHECK(fx.text_of("SELECT state FROM jobs WHERE id = ?", job) != "pending");
    const auto v = fx.message(s.message_id).value();
    CHECK(v.is_draft);
    CHECK(v.trashed);
    CHECK(fx.scalar_of("SELECT COUNT(*) FROM attachments WHERE message_id = ?", s.message_id) == 1);
    CHECK(fx.folder_ids(Folder::Scheduled).empty());
    // Delete forever then removes the (canceled) draft for good; nothing is ever sent.
    actions(fx, {s.thread_id}, ThreadAction::DeleteForever);
    CHECK_FALSE(fx.message(s.message_id).has_value());
    CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, s.outbound_id); }));
  }
  SECTION("trash during the undo window does not unsend; delete forever then cancels") {
    fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE user_settings SET undo_send_seconds = 30"); });
    const SendResult s = fx.send(fx.simple_draft({kBob}));
    actions(fx, {s.thread_id}, ThreadAction::Trash);
    CHECK(status_of(fx, s.outbound_id) == "queued");
    CHECK_FALSE(fx.message(s.message_id)->is_draft);
    actions(fx, {s.thread_id}, ThreadAction::DeleteForever);
    CHECK(status_of(fx, s.outbound_id) == "canceled");
    CHECK_FALSE(fx.message(s.message_id).has_value());
    CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.canceled'",
                       s.outbound_id) == 1);
  }
  SECTION("a send Resend holds is refused with 409 scheduled_send_pending; nothing changes") {
    const SendResult s = fx.send(fx.simple_draft({kExternal}), azm::now_ms() + 2 * kHour);
    REQUIRE(fx.outbound(s.outbound_id).scheduled_via == ScheduledVia::Resend);
    fx.accept(s.outbound_id, "re_sched", true);
    const ApiError e = api_error([&] { actions(fx, {s.thread_id}, ThreadAction::Trash); });
    CHECK(e.status == 409);
    CHECK(e.code == "scheduled_send_pending");
    CHECK_FALSE(fx.message(s.message_id)->trashed);
    CHECK(status_of(fx, s.outbound_id) == "scheduled");
    CHECK(fx.folder_ids(Folder::Scheduled) == std::vector<int64_t>{s.thread_id});
    // Put into Trash some other way (e.g. before this fix): delete forever is refused too.
    fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE messages SET trashed_at = 1 WHERE id = ?", s.message_id); });
    CHECK(api_error([&] { actions(fx, {s.thread_id}, ThreadAction::DeleteForever); }).code == "scheduled_send_pending");
    CHECK(fx.message(s.message_id).has_value());
    // purge_trash leaves it alone as well.
    CHECK(fx.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, azm::now_ms(), 0, 30, 100); }).messages_deleted == 0);
  }
  SECTION("delete forever of a send being POSTed is refused with 409 send_in_progress") {
    const SendResult s = fx.send(fx.simple_draft({kBob}));
    fx.ts.db.write([&](db::Tx& tx) {
      REQUIRE(mark_sending(tx, s.outbound_id));
      tx.run("UPDATE messages SET trashed_at = 1 WHERE id = ?", s.message_id);
    });
    CHECK(api_error([&] { actions(fx, {s.thread_id}, ThreadAction::DeleteForever); }).code == "send_in_progress");
    CHECK(fx.message(s.message_id).has_value());
  }
}

TEST_CASE("R2: purge_trash cancels a queued send before deleting its copy", "[review][R2]") {
  SendFx fx;
  const auto att = fx.upload("a.pdf", "application/pdf", false);
  DraftInput in;
  in.to = std::vector<Address>{kExternal};
  in.attachment_ids = std::vector<int64_t>{att.id};
  const SendResult s = fx.send(fx.create(in), azm::now_ms() + 24 * kHour);
  // Spam (or a trash from before this fix) leaves the queued send behind its removed copy.
  actions(fx, {s.thread_id}, ThreadAction::Spam);
  CHECK(status_of(fx, s.outbound_id) == "queued");
  const auto res = fx.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, azm::now_ms() + 31 * kDay, 30, 30, 100); });
  CHECK(res.messages_deleted == 1);
  CHECK(status_of(fx, s.outbound_id) == "canceled");
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, s.outbound_id); }));
}

TEST_CASE("R2: a send whose attachment rows or sender copy are gone is never sent", "[review][R2]") {
  SendFx fx;
  const auto a1 = fx.upload("a.pdf", "application/pdf", false);
  const auto a2 = fx.upload("b.pdf", "application/pdf", false);
  DraftInput in;
  in.to = std::vector<Address>{kExternal};
  in.attachment_ids = std::vector<int64_t>{a1.id, a2.id};
  const SendResult s = fx.send(fx.create(in));
  CHECK(fx.plan(s.outbound_id).attachments.size() == 2);
  fx.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM attachments WHERE id = ?", a2.id); });
  try {
    (void)fx.plan(s.outbound_id);
    FAIL("a partial attachment set must not be sent");
  } catch (const UnsendableOutbound& e) {
    CHECK(e.name == "attachment_missing");
  }

  const SendResult s2 = fx.send(fx.simple_draft({kBob}));
  fx.ts.db.write([&](db::Tx& tx) { tx.run("DELETE FROM messages WHERE id = ?", s2.message_id); });
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, s2.outbound_id); }));
  CHECK(status_of(fx, s2.outbound_id) == "canceled");
}

// =============================================================================================
// R3 — raw .eml garbage collection
// =============================================================================================

TEST_CASE("R3: the raw .eml is collectable once every copy is gone", "[review][R3]") {
  SendFx fx;
  auto e = SendFx::inbound("rcv-raw", kCustomer, {kAlice, kBob}, {"alice@team.example", "bob@team.example"}, "原文");
  const std::string sha = crypto::sha256_hex("raw-eml-bytes");
  e.raw = BlobRef{sha, 13, "local"};
  const auto r = fx.deliver(e);
  REQUIRE(r.copies.size() == 2);
  auto unreferenced = [&] { return fx.ts.db.read([&](db::Conn& c) { return is_blob_unreferenced(c, sha); }); };
  CHECK_FALSE(unreferenced());
  for (const auto& copy : r.copies) {
    actions(fx, {copy.thread_id}, ThreadAction::Trash, copy.owner_id);
    CHECK_FALSE(unreferenced());  // trashed copies still reference it
    actions(fx, {copy.thread_id}, ThreadAction::DeleteForever, copy.owner_id);
  }
  CHECK(unreferenced());
  CHECK(fx.ts.db.read([&](db::Conn& c) { return unreferenced_blobs(c, azm::now_ms() + kDay, 10); }).size() == 1);
  CHECK(fx.ts.db.write([&](db::Tx& tx) { return forget_blob_if_unreferenced(tx, sha); }));
  CHECK(fx.text_of("SELECT raw_sha256 FROM inbound_emails WHERE resend_id = 'rcv-raw'") == "<null>");
  CHECK(fx.text_of("SELECT state FROM inbound_emails WHERE resend_id = 'rcv-raw'") == "delivered");

  // A raw of an inbound row still being processed stays referenced.
  const std::string sha2 = crypto::sha256_hex("pending-raw");
  fx.ts.db.write([&](db::Tx& tx) {
    register_blob(tx, BlobRef{sha2, 11, "local"}, 1);
    tx.run("INSERT INTO inbound_emails(resend_id, state, source, raw_sha256, created_at, updated_at) "
           "VALUES('rcv-pending', 'pending', 'webhook', ?, 1, 1)",
           sha2);
  });
  CHECK_FALSE(fx.ts.db.read([&](db::Conn& c) { return is_blob_unreferenced(c, sha2); }));
}

// =============================================================================================
// R4 — stale failed rows cannot be retried into a second delivery
// =============================================================================================

TEST_CASE("R4: admin retry of a superseded or copy-less failed row is refused", "[review][R4]") {
  SendFx fx;
  auto failed_send = [&] {
    const SendResult s = fx.send(fx.simple_draft({kExternal}));
    fx.ts.db.write([&](db::Tx& tx) {
      REQUIRE(mark_sending(tx, s.outbound_id));
      mark_failed(tx, s.outbound_id, "monthly_quota_exceeded", "发送配额已用完");
    });
    return s;
  };
  const SendResult s = failed_send();
  const SendResult retried = fx.ts.db.write([&](db::Tx& tx) { return retry_failed_send(tx, fx.alice, s.message_id, 0); });
  CHECK(retried.outbound_id != s.outbound_id);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.superseded'",
                     s.outbound_id) == 1);
  const int64_t rows = fx.scalar("SELECT COUNT(*) FROM outbound");
  ApiError e = api_error([&] { fx.ts.db.write([&](db::Tx& tx) { admin_retry_outbound(tx, s.outbound_id, 0); }); });
  CHECK(e.status == 409);
  CHECK(e.code == "invalid_state");
  CHECK(fx.scalar("SELECT COUNT(*) FROM outbound") == rows);

  // The same failed row retried twice by an admin (double click): only the first one sends.
  const SendResult s2 = failed_send();
  CHECK_NOTHROW(fx.ts.db.write([&](db::Tx& tx) { return admin_retry_outbound(tx, s2.outbound_id, 0); }));
  e = api_error([&] { fx.ts.db.write([&](db::Tx& tx) { admin_retry_outbound(tx, s2.outbound_id, 0); }); });
  CHECK(e.code == "invalid_state");
}

// =============================================================================================
// R5 — spam retention counts from when the copy became spam
// =============================================================================================

TEST_CASE("R5: old-dated spam is not purged right after it arrives or is reported", "[review][R5]") {
  SendFx fx;
  const int64_t now = azm::now_ms();
  auto e = SendFx::inbound("rcv-oldspam", kCustomer, {kAlice}, {"alice@team.example"}, "旧邮件");
  e.date = now - 60 * kDay;
  e.auth = AuthResults{"fail", "fail", "fail"};  // DMARC fail → spam
  const auto r = fx.deliver(e);
  REQUIRE(r.copies.size() == 1);
  REQUIRE(r.copies[0].is_spam);
  // A legitimately received 40-day-old thread the user now reports as spam.
  const auto old = fx.deliver([&] {
    auto x = SendFx::inbound("rcv-old", kCustomer, {kAlice}, {"alice@team.example"}, "老对话");
    x.date = now - 40 * kDay;
    return x;
  }());
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE messages SET created_at = ?, updated_at = ? WHERE id = ?", now - 40 * kDay, now - 40 * kDay,
           old.copies[0].message_id);
  });
  actions(fx, {old.copies[0].thread_id}, ThreadAction::Spam);

  auto purge = [&](int64_t at) {
    return fx.ts.db.write([&](db::Tx& tx) { return purge_trash(tx, at, 30, 30, 100); }).messages_deleted;
  };
  CHECK(purge(now + kHour) == 0);
  CHECK(fx.message(r.copies[0].message_id).has_value());
  CHECK(fx.message(old.copies[0].message_id).has_value());
  CHECK(purge(now + 31 * kDay) == 2);  // 30 days after arrival / report
}

// =============================================================================================
// R6 / RT-3 — the wire headers are frozen before the first POST
// =============================================================================================

TEST_CASE("RT-3/R6: In-Reply-To/References never change under the same Idempotency-Key", "[review][RT-3][R6]") {
  SendFx fx;
  // Alice's own sent mail whose Message-ID is not captured yet.
  const SendResult parent = fx.send(fx.simple_draft({kExternal}, "进度"));
  fx.accept(parent.outbound_id, "re_parent");
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(parent.message_id);
  in.html = "<p>补充</p>";
  const SendResult reply = fx.send(fx.create(in));
  CHECK(fx.plan(reply.outbound_id).parent_message_id_missing);

  SECTION("domain level: freeze, then a late capture changes nothing") {
    CHECK(fx.ts.db.write([&](db::Tx& tx) { return freeze_send_headers(tx, reply.outbound_id); }));
    CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return freeze_send_headers(tx, reply.outbound_id); }));
    const auto before = fx.plan(reply.outbound_id);
    fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, parent.outbound_id, "parent-1@resend.dev"); });
    const auto after = fx.plan(reply.outbound_id);
    CHECK(after.headers_frozen);
    CHECK_FALSE(after.parent_message_id_missing);
    CHECK(after.headers == before.headers);
    CHECK(header(after, "In-Reply-To").empty());
    // A retry clone (new uuid = new key) resolves afresh and picks the parent id up.
    fx.ts.db.write([&](db::Tx& tx) {
      REQUIRE(mark_sending(tx, reply.outbound_id));
      mark_failed(tx, reply.outbound_id, "x", "失败");
    });
    const auto r2 = fx.ts.db.write([&](db::Tx& tx) { return retry_failed_send(tx, fx.alice, reply.message_id, 0); });
    CHECK(header(fx.plan(r2.outbound_id), "In-Reply-To") == "<parent-1@resend.dev>");
  }
  SECTION("job level: a lost response, then a captured parent id: the retry body is identical") {
    FakeResend fake;
    fx.ts.svc.resend = &fake;  // GET re_parent → 404: sent without the parent id
    fake.send_results.push_back(resend::Error(resend::Error::Kind::Network, 0, "network", "timeout"));
    std::stop_source ss;
    CHECK_THROWS_AS(jobs::run_outbound_send(fx.ts.svc, send_job(reply.outbound_id, 1), ss.get_token()), jobs::Retry);
    fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, parent.outbound_id, "parent-1@resend.dev"); });
    fake.emails["re_parent"] = resend::SentEmail{"re_parent", "<parent-1@resend.dev>", "sent", {}, 0};
    jobs::run_outbound_send(fx.ts.svc, send_job(reply.outbound_id, 2), ss.get_token());
    REQUIRE(fake.sends.size() == 2);
    CHECK(fake.sends[0].idempotency_key == fake.sends[1].idempotency_key);
    CHECK(fake.sends[0].headers == fake.sends[1].headers);
    CHECK(fake.sends[0].html == fake.sends[1].html);
    CHECK(fx.outbound(reply.outbound_id).status == OutboundStatus::Accepted);
    fx.ts.svc.resend = nullptr;
  }
}

// =============================================================================================
// R7 / SEC-3 — a replayed X-AzMail-Ref proves nothing by itself
// =============================================================================================

TEST_CASE("R7/SEC-3: a forged mail with a known X-AzMail-Ref is still spoofed internal", "[review][R7][SEC-3]") {
  SendFx fx;
  const SendResult s = fx.send(fx.simple_draft({kExternal}, "报价"));
  fx.accept(s.outbound_id, "re_ext");
  const std::string uuid = fx.outbound(s.outbound_id).uuid;

  // ext@ forges alice to bob (not a recipient of that send), no DKIM/DMARC verdict.
  auto forged = SendFx::inbound("rcv-forged", kAlice, {kBob}, {"bob@team.example"}, "请转账", "forged@evil.example");
  forged.x_azmail_ref = uuid;
  forged.auth = AuthResults{};
  const auto r = fx.deliver(forged);
  REQUIRE(r.copies.size() == 1);
  CHECK(r.copies[0].is_spam);
  CHECK_FALSE(r.copies[0].loopback_merged);
  CHECK_FALSE(fx.outbound(s.outbound_id).message_id_header.has_value());  // never captured from it

  // An unauthenticated ref match whose recipients are those of the send is our loopback (not
  // spoofed), but it does not capture the Message-ID either.
  const SendResult s2 = fx.send(fx.simple_draft({kBob}, "内部"));
  fx.accept(s2.outbound_id, "re_int");
  auto loop = SendFx::inbound("rcv-loop", kAlice, {kBob}, {"bob@team.example"}, "内部", "loop@resend.dev");
  loop.x_azmail_ref = fx.outbound(s2.outbound_id).uuid;
  loop.auth = AuthResults{"pass", "fail", "none"};
  const auto r2 = fx.deliver(loop);
  REQUIRE(r2.copies.size() == 1);
  CHECK_FALSE(r2.copies[0].is_spam);
  CHECK_FALSE(fx.outbound(s2.outbound_id).message_id_header.has_value());
}

TEST_CASE("SEC-3: the late-loopback cleanup never deletes somebody else's message", "[review][SEC-3]") {
  SendFx fx;
  // A genuine inbound message in alice's mailbox carrying Message-ID X.
  const auto genuine = fx.deliver(
      SendFx::inbound("rcv-genuine", kCustomer, {kAlice}, {"alice@team.example"}, "真实来信", "x@customer.example"));
  REQUIRE(genuine.copies.size() == 1);
  const SendResult s = fx.send(fx.simple_draft({kAlice}, "自己"));
  fx.accept(s.outbound_id, "re_self");
  fx.ts.db.write([&](db::Tx& tx) { set_outbound_message_id(tx, s.outbound_id, "x@customer.example"); });
  CHECK(fx.message(genuine.copies[0].message_id).has_value());
}

// =============================================================================================
// R8 — reconcile rotates over every candidate
// =============================================================================================

TEST_CASE("R8: outbound_to_reconcile_after walks every candidate round robin", "[review][R8]") {
  SendFx fx;
  const int64_t now = azm::now_ms();
  std::vector<int64_t> ids;
  fx.ts.db.write([&](db::Tx& tx) {
    for (int i = 0; i < 5; ++i) {
      tx.run("INSERT INTO outbound(uuid, sender_user_id, from_address_id, status, send_after, payload_json, "
             "resend_id, last_event_at, created_at, updated_at) VALUES(?, ?, ?, 'sent', 1, '{}', ?, 1, ?, ?)",
             "u" + std::to_string(i), fx.alice, fx.alice_addr, "re_" + std::to_string(i), now, now);
      ids.push_back(tx.last_insert_id());
    }
  });
  auto batch = [&](int64_t after) {
    std::vector<int64_t> out;
    for (const auto& it : fx.ts.db.read([&](db::Conn& c) { return outbound_to_reconcile_after(c, now, kDay, 2, after); }))
      out.push_back(it.outbound_id);
    return out;
  };
  CHECK(batch(0) == std::vector<int64_t>{ids[0], ids[1]});
  CHECK(batch(ids[1]) == std::vector<int64_t>{ids[2], ids[3]});
  CHECK(batch(ids[3]) == std::vector<int64_t>{ids[4], ids[0]});  // wraps around
}

TEST_CASE("R8: a reconcile poll that restates a webhook event adds no second log row", "[review][R8]") {
  SendFx fx;
  const SendResult s = fx.send(fx.simple_draft({kExternal}));
  fx.accept(s.outbound_id, "re_poll");
  CHECK(fx.event("re_poll", "email.delivered", "svix_1").outcome == EventOutcome::Applied);
  const auto again = fx.event("re_poll", "email.delivered", "poll:delivered", 0, {}, {}, "<p1@resend.dev>");
  CHECK(again.outcome == EventOutcome::Duplicate);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'email.delivered'",
                     s.outbound_id) == 1);
  CHECK(fx.outbound(s.outbound_id).message_id_header == "p1@resend.dev");  // still captured
  // A poll that reports something new is applied as before.
  CHECK(fx.event("re_poll", "email.bounced", "poll:bounced").outcome == EventOutcome::Applied);
  CHECK(fx.outbound(s.outbound_id).status == OutboundStatus::Bounced);
}

// =============================================================================================
// R9 — a scheduled send is dated when it goes out
// =============================================================================================

TEST_CASE("R9: scheduled copies get the real send time as their date", "[review][R9]") {
  SendFx fx;
  SECTION("local scheduling: at mark_accepted") {
    const auto att = fx.upload("a.pdf", "application/pdf", false);
    DraftInput in;
    in.to = std::vector<Address>{kExternal};
    in.attachment_ids = std::vector<int64_t>{att.id};
    const SendResult s = fx.send(fx.create(in), azm::now_ms() + 2 * kHour);
    REQUIRE(fx.outbound(s.outbound_id).scheduled_via == ScheduledVia::Local);
    fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE messages SET date = 1000 WHERE id = ?", s.message_id); });
    const int64_t before = azm::now_ms();
    fx.accept(s.outbound_id, "re_local");
    CHECK(fx.message(s.message_id)->date >= before);
  }
  SECTION("Resend scheduling: at email.sent") {
    const SendResult s = fx.send(fx.simple_draft({kExternal}), azm::now_ms() + 2 * kHour);
    fx.accept(s.outbound_id, "re_rs", true);
    fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE messages SET date = 1000 WHERE id = ?", s.message_id); });
    fx.event("re_rs", "email.scheduled", "wh:1");
    CHECK(fx.message(s.message_id)->date == 1000);  // not sent yet
    const int64_t at = azm::now_ms() - 5000;
    fx.event("re_rs", "email.sent", "wh:2", at);
    CHECK(fx.message(s.message_id)->date == at);
  }
}

// =============================================================================================
// R10 — Resend reports the scheduled send canceled
// =============================================================================================

TEST_CASE("R10: email.canceled on a Resend-scheduled row finishes the cancel", "[review][R10]") {
  SendFx fx;
  const SendResult s = fx.send(fx.simple_draft({kExternal}, "定时", "<p>定时正文</p>"), azm::now_ms() + 2 * kHour);
  fx.accept(s.outbound_id, "re_cx", true);
  const auto res = fx.event("re_cx", "email.canceled", "poll:canceled");
  CHECK(res.status_changed);
  CHECK(fx.outbound(s.outbound_id).status == OutboundStatus::Canceled);
  const auto v = fx.message(s.message_id).value();
  CHECK(v.is_draft);
  CHECK(fx.draft(s.message_id)->html.find("定时正文") != std::string::npos);
  CHECK(fx.folder_ids(Folder::Scheduled).empty());
  CHECK(fx.folder_ids(Folder::Drafts) == std::vector<int64_t>{s.thread_id});
}

TEST_CASE("R10: a cancel whose earlier response was lost completes instead of 409", "[review][R10]") {
  SendFx fx;
  FakeResend fake;
  fx.ts.svc.resend = &fake;
  const SendResult s = fx.send(fx.simple_draft({kExternal}, "定时", "<p>定时正文</p>"), azm::now_ms() + 2 * kHour);
  fx.accept(s.outbound_id, "re_lost", true);
  fake.cancel_error = resend::Error(resend::Error::Kind::Validation, 422, "validation_error", "already canceled");
  auto call = [&] {
    http::Request req = http::Request::make(boost::beast::http::verb::post, "/");
    http::Params params;
    params.emplace("id", std::to_string(s.message_id));
    http::Ctx ctx{req, fx.ts.svc, std::move(params), http::Principal{fx.alice, 1, true, kAlice.email}};
    try {
      return api::messages_cancel_schedule(ctx);
    } catch (const ApiError& e) {
      return http::Response::from_error(e);
    }
  };
  // Resend still has it scheduled (a real "already sent" case): 409 already_sent as before.
  fake.emails["re_lost"] = resend::SentEmail{"re_lost", std::nullopt, "scheduled", {}, 0};
  CHECK(call().status == 409u);
  CHECK(fx.outbound(s.outbound_id).status == OutboundStatus::Scheduled);
  // Resend reports it canceled: the cancel is finished locally.
  fake.emails["re_lost"].last_event = "canceled";
  CHECK(call().status == 200u);
  CHECK(fx.outbound(s.outbound_id).status == OutboundStatus::Canceled);
  CHECK(fx.message(s.message_id)->is_draft);
  fx.ts.svc.resend = nullptr;
}

// =============================================================================================
// R11 — split delivery of a loopback merges once
// =============================================================================================

TEST_CASE("R11: a second part of a split loopback does not make the copy unread again", "[review][R11]") {
  SendFx fx;
  DraftInput in;
  in.to = std::vector<Address>{kSupport, kBob};
  in.from_address_id = fx.support;
  in.subject = "分批";
  const SendResult s = fx.send(fx.create(in));
  fx.accept(s.outbound_id, "re_split");
  const std::string uuid = fx.outbound(s.outbound_id).uuid;
  const int64_t bob_copy =
      fx.scalar_of("SELECT id FROM messages WHERE owner_id = ? AND outbound_id = ?", fx.bob, s.outbound_id);

  auto part = [&](std::string id, std::vector<std::string> rcpts) {
    auto e = SendFx::inbound(std::move(id), kSupport, {kSupport, kBob}, std::move(rcpts), "分批", "split@resend.dev");
    e.x_azmail_ref = uuid;
    return fx.deliver(e);
  };
  fx.ts.notifier.clear();
  part("rcv-part1", {"bob@team.example"});
  CHECK(fx.events_of(ws::events::kMailNew, fx.bob).size() == 1);
  fx.ts.db.write([&](db::Tx& tx) { patch_message(tx, fx.bob, bob_copy, MessagePatch{true, {}, {}, {}}); });
  actions(fx, {fx.message(bob_copy, fx.bob)->thread_id}, ThreadAction::Archive, fx.bob);
  const auto r2 = part("rcv-part2", {"support@team.example"});
  CHECK(r2.state == DeliveryResult::State::Delivered);
  const auto v = fx.message(bob_copy, fx.bob).value();
  CHECK(v.is_read);
  CHECK_FALSE(v.in_inbox);  // stays archived
  CHECK(fx.events_of(ws::events::kMailNew, fx.bob).size() == 1);
}

// =============================================================================================
// R12 — has_attachments follows the view's messages
// =============================================================================================

TEST_CASE("R12: a trashed message's attachment puts no paperclip on the Inbox row", "[review][R12]") {
  SendFx fx;
  Msg with_att;
  with_att.owner = fx.alice;
  with_att.from = kCustomer;
  with_att.to = {kAlice};
  with_att.subject = "附件";
  with_att.message_id = "att@customer.example";
  with_att.atts = {Att{}};
  with_att.date = azm::now_ms() - 2 * kHour;
  const auto m1 = fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_message(tx, with_att); });
  actions(fx, {m1.thread_id}, ThreadAction::Trash);
  // A reply arrives into the same thread.
  auto e = SendFx::inbound("rcv-re", kCustomer, {kAlice}, {"alice@team.example"}, "Re: 附件", "re@customer.example");
  e.in_reply_to = "att@customer.example";
  const auto r = fx.deliver(e);
  REQUIRE(r.copies[0].thread_id == m1.thread_id);
  const ThreadPage inbox_page = fx.folder(Folder::Inbox);
  const auto* inbox = item_of(inbox_page, m1.thread_id);
  REQUIRE(inbox != nullptr);
  CHECK_FALSE(inbox->has_attachments);
  CHECK(inbox->attachments_preview.empty());
  const ThreadPage trash_page = fx.folder(Folder::Trash);
  const auto* trash = item_of(trash_page, m1.thread_id);
  REQUIRE(trash != nullptr);
  CHECK(trash->has_attachments);
  CHECK(trash->attachments_preview.size() == 1);
}

// =============================================================================================
// SEC-1 / SEC-5 — linear-time text conversion and header decoding
// =============================================================================================

TEST_CASE("SEC-1: html_to_text / make_snippet stay linear on crafted HTML", "[review][SEC-1]") {
  const std::string heads = repeat("<head><body>", 512 * 1024 / 12);
  CHECK(seconds_of([&] { (void)html_to_text(heads); }) < 5.0);
  CHECK(seconds_of([&] { (void)make_snippet(heads, true); }) < 5.0);
  const std::string anchors =
      repeat("<a href=\"https://x.example/\">", 20000) + repeat("文字 ", 50000) + repeat("</a>", 20000);
  CHECK(seconds_of([&] { (void)html_to_text(anchors); }) < 5.0);
  const std::string pre = "<pre>" + repeat("\n<p>x</p>", 100000) + "</pre>";
  CHECK(seconds_of([&] { (void)html_to_text(pre); }) < 5.0);
  // Behaviour kept: a missing </head> still stops at <body; a link's target is shown once.
  CHECK(html_to_text("<head><title>t</title><body><p>正文</p>") == "正文");
  CHECK(html_to_text("<html><head><style>p{}</style></head><body>A</body></html>") == "A");
  CHECK(html_to_text("<a href=\"https://a.example/x\">点这里</a>") == "点这里 <https://a.example/x>");
}

TEST_CASE("SEC-5: RFC 2047 decoding of unterminated encoded-words is linear", "[review][SEC-5]") {
  const std::string subject = repeat("=?x?b?QQ ", 512 * 1024 / 9);
  CHECK(seconds_of([&] { (void)decode_rfc2047(subject); }) < 5.0);
  const std::string raw = "Subject: " + subject + "\r\nFrom: =?utf-8?b?5byg5LiJ?= <a@x.example>\r\n\r\n";
  CHECK(seconds_of([&] { (void)parse_headers(raw); }) < 5.0);
  CHECK(decode_rfc2047("=?utf-8?b?5byg5LiJ?=") == "张三");
  CHECK(decode_rfc2047("=?utf-8?q?a_b?= =?utf-8?q?c?=") == "a bc");
  CHECK(decode_rfc2047("=?utf-8?q?ok?" "?=") == "ok?");  // a raw '?' from a sloppy encoder
}

// =============================================================================================
// SEC-4 — send-as and account status are re-checked at send / retry time
// =============================================================================================

TEST_CASE("SEC-4: a revoked send-as or disabled sender stops scheduled sends and retries", "[review][SEC-4]") {
  SendFx fx;
  DraftInput in;
  in.to = std::vector<Address>{kExternal};
  in.from_address_id = fx.support;
  const SendResult s = fx.send(fx.create(in));
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE alias_members SET can_send_as = 0 WHERE user_id = ?", fx.alice); });
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, s.outbound_id); }));
  const auto row = fx.outbound(s.outbound_id);
  CHECK(row.status == OutboundStatus::Failed);
  CHECK(fx.text_of("SELECT error_name FROM outbound WHERE id = ?", s.outbound_id) == "sender_not_allowed");
  const ApiError e =
      api_error([&] { fx.ts.db.write([&](db::Tx& tx) { retry_failed_send(tx, fx.alice, s.message_id, 0); }); });
  CHECK(e.status == 403);
  CHECK(e.code == "send_as_forbidden");

  const SendResult own = fx.send(fx.simple_draft({kExternal}));
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE users SET disabled = 1 WHERE id = ?", fx.alice); });
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, own.outbound_id); }));
  CHECK(fx.outbound(own.outbound_id).status == OutboundStatus::Failed);
}

// =============================================================================================
// F4 — the server never appends the settings signature
// =============================================================================================

TEST_CASE("F4: the body is sent exactly as the editor holds it (no server-side signature)", "[review][F4]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE user_settings SET signature_enabled = 1, signature_html = '<p>— 张三</p>' WHERE user_id = ?",
           fx.alice);
  });
  // 不使用签名: the editor removed the block.
  const auto p1 = fx.plan(fx.send(fx.simple_draft({kBob}, "s", "<p>正文</p>")).outbound_id);
  CHECK(p1.html.find("张三") == std::string::npos);
  CHECK(p1.html.find("azm-signature") == std::string::npos);
  // Edited for this message: sent once, as edited.
  const auto p2 = fx.plan(
      fx.send(fx.simple_draft({kBob}, "s", "<p>正文</p><div data-azm-signature=\"\"><p>— 张</p></div>")).outbound_id);
  CHECK(count_of(p2.html, "— 张") == 1);
  CHECK(p2.html.find("张三") == std::string::npos);
  // Image-only signature already in the body: once.
  fx.ts.db.write([&](db::Tx& tx) {
    tx.run("UPDATE user_settings SET signature_html = '<img src=\"https://cdn.example/logo.png\">' WHERE user_id = ?",
           fx.alice);
  });
  const auto p3 = fx.plan(fx.send(fx.simple_draft({kBob}, "s",
                                                  "<p>正文</p><div data-azm-signature=\"\"><img "
                                                  "src=\"https://cdn.example/logo.png\"></div>"))
                              .outbound_id);
  CHECK(count_of(p3.html, "https://cdn.example/logo.png") == 1);
}
