// Owner: WP-B — fixtures for the write-side mail tests (drafts, outbound, delivery).
//
// A team domain with alice, bob, carol, dave and the alias support@ (alice may send as it, bob is
// a plain member, share_sent=1), plus helpers that run the real domain functions in their own
// write transactions.
#pragma once

#include "core/crypto.hpp"
#include "core/errors.hpp"
#include "mail/attachments.hpp"
#include "mail/drafts.hpp"
#include "mail/inbound.hpp"
#include "mail/mailbox.hpp"
#include "mail/outbound.hpp"
#include "mail/serde.hpp"
#include "mail_fixtures.hpp"
#include "test_support.hpp"

#include <boost/json/object.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace azm::test::sendfx {

using mail::DraftInput;

inline const Address kAlice{"Alice", "alice@team.example"};
inline const Address kBob{"Bob", "bob@team.example"};
inline const Address kCarol{"Carol", "carol@team.example"};
inline const Address kDave{"Dave", "dave@team.example"};
inline const Address kSupport{"客服", "support@team.example"};
inline const Address kCustomer{"王五", "wang@customer.example"};
inline const Address kExternal{"Ext", "ext@other.example"};

// Runs `f` and returns the ApiError it throws (fails the test when nothing / something else is thrown).
inline ApiError api_error(const std::function<void()>& f) {
  try {
    f();
  } catch (const ApiError& e) {
    return e;
  }
  FAIL("expected an ApiError");
  return ApiError(0, "", "");
}

struct SendFx {
  test::TestServices ts;
  int64_t alice = 0, bob = 0, carol = 0, dave = 0;
  int64_t alice_addr = 0, bob_addr = 0, carol_addr = 0, support = 0;

  SendFx() {
    ts.db.write([&](db::Tx& tx) {
      alice = test::seed_user(tx, kAlice.email, true, kAlice.name);
      bob = test::seed_user(tx, kBob.email, false, kBob.name);
      carol = test::seed_user(tx, kCarol.email, false, kCarol.name);
      dave = test::seed_user(tx, kDave.email, false, kDave.name);
      alice_addr = test::address_id(tx.conn(), kAlice.email);
      bob_addr = test::address_id(tx.conn(), kBob.email);
      carol_addr = test::address_id(tx.conn(), kCarol.email);
      support = test::seed_alias(tx, kSupport.email, {{alice, true}, {bob, false}}, true, kSupport.name);
      // Undo disabled by default so sends are easy to reason about; tests opt in.
      tx.run("UPDATE user_settings SET undo_send_seconds = 0, signature_enabled = 0");
    });
  }

  // ---- drafts ----------------------------------------------------------------------------------
  mail::Draft create(DraftInput in, int64_t owner = 0) {
    return ts.db.write([&](db::Tx& tx) { return mail::create_draft(tx, ts.urls, owner ? owner : alice, in); });
  }
  mail::Draft update(int64_t id, int64_t version, DraftInput in, bool force = false, int64_t owner = 0) {
    return ts.db.write(
        [&](db::Tx& tx) { return mail::update_draft(tx, ts.urls, owner ? owner : alice, id, version, in, force); });
  }
  std::optional<mail::Draft> draft(int64_t id, int64_t owner = 0) {
    return ts.db.read([&](db::Conn& c) { return mail::get_draft(c, ts.urls, owner ? owner : alice, id); });
  }
  // A draft to `to` (subject/html as given) ready to send.
  mail::Draft simple_draft(std::vector<Address> to, std::string subject = "测试邮件", std::string html = "<p>你好</p>",
                           int64_t owner = 0) {
    DraftInput in;
    in.to = std::move(to);
    in.subject = std::move(subject);
    in.html = std::move(html);
    return create(in, owner);
  }

  // ---- send ------------------------------------------------------------------------------------
  mail::SendResult send(const mail::Draft& d, std::optional<int64_t> scheduled_at = {}, int64_t owner = 0) {
    mail::SendOptions o;
    o.version = d.version;
    o.scheduled_at = scheduled_at;
    return ts.db.write(
        [&](db::Tx& tx) { return mail::queue_send(tx, ts.cfg, ts.urls, owner ? owner : alice, d.id, o); });
  }
  mail::SendResult send_opts(int64_t draft_id, mail::SendOptions o, int64_t owner = 0) {
    return ts.db.write(
        [&](db::Tx& tx) { return mail::queue_send(tx, ts.cfg, ts.urls, owner ? owner : alice, draft_id, o); });
  }
  mail::OutboundSendPlan plan(int64_t outbound_id) {
    return ts.db.read([&](db::Conn& c) { return mail::load_send_plan(c, outbound_id); });
  }
  mail::OutboundRow outbound(int64_t id) {
    return ts.db.read([&](db::Conn& c) { return mail::get_outbound(c, id); }).value();
  }
  // Simulates the outbound.send job up to acceptance.
  void accept(int64_t outbound_id, std::string resend_id, bool scheduled = false) {
    ts.db.write([&](db::Tx& tx) {
      REQUIRE(mail::mark_sending(tx, outbound_id));
      mail::mark_accepted(tx, outbound_id, resend_id, scheduled);
    });
  }
  mail::EventApplyResult event(std::optional<std::string> resend_id, std::string type, std::string source_key,
                               int64_t occurred_at = 0, boost::json::object detail = {},
                               std::optional<std::string> uuid = {}, std::optional<std::string> message_id = {}) {
    mail::OutboundEvent ev;
    ev.resend_id = std::move(resend_id);
    ev.uuid = std::move(uuid);
    ev.type = std::move(type);
    ev.occurred_at = occurred_at;
    ev.detail = std::move(detail);
    ev.source_key = std::move(source_key);
    ev.message_id = std::move(message_id);
    return ts.db.write([&](db::Tx& tx) { return mail::apply_outbound_event(tx, ev); });
  }

  // ---- inbound ---------------------------------------------------------------------------------
  static mail::InboundEmail inbound(std::string resend_id, Address from, std::vector<Address> to,
                                    std::vector<std::string> received_for, std::string subject = "来信",
                                    std::optional<std::string> message_id = {}) {
    mail::InboundEmail e;
    e.resend_id = std::move(resend_id);
    e.from = std::move(from);
    e.to = std::move(to);
    e.received_for = std::move(received_for);
    e.subject = std::move(subject);
    e.text = "正文内容";
    e.html = "<p>正文内容</p>";
    e.message_id = std::move(message_id);
    e.date = azm::now_ms() - 1000;
    e.received_at = azm::now_ms();
    e.auth = mail::AuthResults{"pass", "pass", "pass"};
    return e;
  }
  mail::DeliveryResult deliver(const mail::InboundEmail& e, mail::DeliveryOptions o = {}) {
    return ts.db.write([&](db::Tx& tx) { return mail::deliver_inbound(tx, e, o); });
  }

  // ---- reads -----------------------------------------------------------------------------------
  std::optional<mail::MessageView> message(int64_t id, int64_t owner = 0) {
    return ts.db.read([&](db::Conn& c) { return mail::get_message(c, ts.urls, owner ? owner : alice, id); });
  }
  std::optional<mail::ThreadDetail> thread(int64_t id, int64_t owner = 0) {
    return ts.db.read([&](db::Conn& c) { return mail::get_thread(c, ts.urls, owner ? owner : alice, id); });
  }
  mail::ThreadPage folder(mail::Folder f, int64_t owner = 0) {
    mail::ThreadQuery q;
    q.folder = f;
    return ts.db.read([&](db::Conn& c) { return mail::list_threads(c, owner ? owner : alice, q); });
  }
  std::vector<int64_t> folder_ids(mail::Folder f, int64_t owner = 0) {
    std::vector<int64_t> out;
    for (const auto& t : folder(f, owner).items) out.push_back(t.id);
    return out;
  }
  int64_t scalar(std::string_view sql) {
    return ts.db.read([&](db::Conn& c) { return c.scalar<int64_t>(sql).value_or(-1); });
  }
  template <class... A>
  int64_t scalar_of(std::string_view sql, A... a) {
    return ts.db.read([&](db::Conn& c) { return c.scalar<int64_t>(sql, a...).value_or(-1); });
  }
  template <class... A>
  std::string text_of(std::string_view sql, A... a) {
    return ts.db.read([&](db::Conn& c) { return c.scalar<std::string>(sql, a...).value_or("<null>"); });
  }
  // messages of `owner` (all directions/states), by id.
  std::vector<int64_t> message_ids(int64_t owner) {
    return ts.db.read([&](db::Conn& c) {
      std::vector<int64_t> out;
      auto s = c.prepare("SELECT id FROM messages WHERE owner_id = ? ORDER BY id");
      s.bind_all(owner);
      while (s.step()) out.push_back(s.i64(0));
      return out;
    });
  }
  // An unattached upload (blob row registered; bytes not needed by the DB layer).
  mail::AttachmentView upload(std::string filename, std::string type, bool is_inline, int64_t size = 100,
                              int64_t owner = 0, std::string content = {}) {
    if (content.empty()) content = filename + std::to_string(size);
    return ts.db.write([&](db::Tx& tx) {
      return mail::create_upload(tx, ts.urls, owner ? owner : alice,
                                 BlobRef{crypto::sha256_hex(content), size, "local"}, filename, type, is_inline,
                                 azm::now_ms());
    });
  }
  std::vector<RecordingNotifier::Event> events_of(std::string_view type, int64_t user) {
    std::vector<RecordingNotifier::Event> out;
    for (auto& e : ts.notifier.events_of(type))
      if (e.user_id == user) out.push_back(e);
    return out;
  }
};

// Header value from a plan ("" when absent).
inline std::string header(const mail::OutboundSendPlan& p, std::string_view name) {
  for (const auto& [k, v] : p.headers)
    if (k == name) return v;
  return {};
}

}  // namespace azm::test::sendfx
