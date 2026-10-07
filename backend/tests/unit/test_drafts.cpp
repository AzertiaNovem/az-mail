// Owner: WP-B — drafts: create/update/delete, optimistic versions (409 with the current draft),
// send-as, stored-HTML cid: invariant round trips, reply/forward initialization (quote images),
// attachment lists, undo-send races, IDOR.
#include "send_fixtures.hpp"

#include "ws/events.hpp"

#include <boost/json/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace azm;
using namespace azm::mail;
using namespace azm::test::sendfx;
using test::mailfx::Att;
using test::mailfx::Msg;

namespace {

std::string stored_html(SendFx& fx, int64_t id) {
  return fx.text_of("SELECT html FROM message_bodies WHERE message_id = ?", id);
}
std::string stored_quoted(SendFx& fx, int64_t id) {
  return fx.text_of("SELECT quoted_html FROM message_bodies WHERE message_id = ?", id);
}

// An inbound message for alice delivered to `delivered_to`, with an inline image referenced by
// the HTML and a regular PDF attachment.
test::mailfx::Inserted parent_with_images(SendFx& fx, std::string delivered_to = "alice@team.example") {
  Msg m;
  m.owner = fx.alice;
  m.from = kCustomer;
  m.to = {kSupport};
  m.cc = {kCarol, kExternal};
  m.reply_to = {};
  m.subject = "订单问题";
  m.message_id = "orig-1@customer.example";
  m.delivered_to = delivered_to;
  m.html = "<p>见图</p><img src=\"cid:logo123\">";
  m.date = azm::now_ms() - 60'000;
  Att img;
  img.filename = "logo.png";
  img.content_type = "image/png";
  img.content_id = "logo123";
  img.is_inline = true;
  img.content = "png-bytes";
  Att pdf;
  pdf.filename = "合同.pdf";
  pdf.content = "pdf-bytes";
  m.atts = {img, pdf};
  return fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_message(tx, m); });
}

}  // namespace

TEST_CASE("create_draft: defaults, new thread, drafts folder, emits", "[drafts]") {
  SendFx fx;
  fx.ts.notifier.clear();
  DraftInput in;
  in.to = std::vector<Address>{kBob};
  in.subject = "周报\r\n注入";
  in.html = "<p>本周进展</p>";
  const Draft d = fx.create(in);
  CHECK(d.version == 1);
  CHECK(d.mode == DraftMode::New);
  CHECK_FALSE(d.parent_message_id);
  CHECK(d.from_address_id == fx.alice_addr);
  CHECK(d.to == std::vector<Address>{kBob});
  CHECK(d.subject == "周报  注入");  // header-safe: CR/LF become spaces
  CHECK(d.html == "<p>本周进展</p>");
  CHECK_FALSE(d.quoted_html);
  CHECK(d.attachments.empty());
  CHECK(d.updated_at > 0);

  CHECK(fx.folder_ids(Folder::Drafts) == std::vector<int64_t>{d.thread_id});
  CHECK(fx.folder_ids(Folder::Inbox).empty());
  CHECK(fx.folder_ids(Folder::Sent).empty());
  const auto agg = fx.ts.db.read([&](db::Conn& c) { return test::mailfx::aggregates(c, d.thread_id); }).value();
  CHECK(agg.draft_count == 1);
  CHECK(agg.msg_count == 0);
  CHECK(agg.snippet == "本周进展");
  const auto changed = fx.events_of(ws::events::kThreadsChanged, fx.alice);
  REQUIRE(changed.size() == 1);
  CHECK(boost::json::serialize(changed[0].data) == "{\"thread_ids\":[" + std::to_string(d.thread_id) + "]}");
  // FTS covers drafts.
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM message_fts WHERE message_fts MATCH ?", std::string("\"本周进展\"")) == 1);
  // The view is a draft, direction out.
  const auto v = fx.message(d.id).value();
  CHECK(v.is_draft);
  CHECK(v.direction == Direction::Out);
  CHECK_FALSE(v.outbound);
}

TEST_CASE("update_draft: version check, 409 with the full current draft, force", "[drafts]") {
  SendFx fx;
  const Draft d = fx.simple_draft({kBob});
  DraftInput a;
  a.subject = "第二版";
  const Draft d2 = fx.update(d.id, d.version, a);
  CHECK(d2.version == d.version + 1);
  CHECK(d2.subject == "第二版");
  CHECK(d2.to == std::vector<Address>{kBob});  // absent fields keep the stored value

  // A stale version (another window saved meanwhile).
  DraftInput b;
  b.subject = "旧窗口";
  const ApiError e = api_error([&] { fx.update(d.id, d.version, b); });
  CHECK(e.status == 409);
  CHECK(e.code == "version_conflict");
  REQUIRE(e.details.contains("current"));
  const auto& cur = e.details.at("current").as_object();
  CHECK(cur.at("id").as_int64() == d.id);
  CHECK(cur.at("version").as_int64() == d2.version);
  CHECK(cur.at("subject").as_string() == "第二版");
  CHECK(cur.at("mode").as_string() == "new");
  CHECK(cur.contains("attachments"));
  CHECK(fx.draft(d.id)->subject == "第二版");  // nothing changed

  // Overwrite (force) ignores the version.
  const Draft d3 = fx.update(d.id, 1, b, /*force=*/true);
  CHECK(d3.subject == "旧窗口");
  CHECK(d3.version == d2.version + 1);

  // quoted_html: null clears, absent keeps.
  DraftInput q;
  q.quoted_html = std::optional<std::string>("<blockquote>原文</blockquote>");
  const Draft d4 = fx.update(d.id, d3.version, q);
  CHECK(d4.quoted_html == "<blockquote>原文</blockquote>");
  const Draft d5 = fx.update(d.id, d4.version, DraftInput{});
  CHECK(d5.quoted_html == "<blockquote>原文</blockquote>");
  DraftInput clear;
  clear.quoted_html = std::optional<std::string>();
  CHECK_FALSE(fx.update(d.id, d5.version, clear).quoted_html);
}

TEST_CASE("drafts: send-as rule and IDOR", "[drafts][idor]") {
  SendFx fx;
  DraftInput as_alias;
  as_alias.from_address_id = fx.support;
  const Draft d = fx.create(as_alias);  // alice may send as support@
  CHECK(d.from_address_id == fx.support);
  CHECK(fx.text_of("SELECT from_email FROM messages WHERE id = ?", d.id) == "support@team.example");
  CHECK(fx.text_of("SELECT from_name FROM messages WHERE id = ?", d.id) == "客服");

  // bob is a member without can_send_as; carol's mailbox is not bob's; unknown ids too.
  for (int64_t addr : {fx.support, fx.carol_addr, int64_t{999999}}) {
    DraftInput in;
    in.from_address_id = addr;
    const ApiError e = api_error([&] { fx.create(in, fx.bob); });
    CHECK(e.status == 403);
    CHECK(e.code == "send_as_forbidden");
  }
  const SenderIdentity own = fx.ts.db.read([&](db::Conn& c) { return resolve_sender(c, fx.bob, fx.bob_addr); });
  CHECK(own.address.email == "bob@team.example");
  CHECK(own.address.name == "Bob");
  CHECK_FALSE(own.is_alias);
  const SenderIdentity alias = fx.ts.db.read([&](db::Conn& c) { return resolve_sender(c, fx.alice, fx.support); });
  CHECK(alias.is_alias);
  CHECK(alias.share_sent);
  CHECK(fx.ts.db.read([&](db::Conn& c) { return default_from_address(c, fx.bob); }) == fx.bob_addr);

  // bob cannot see, change, delete or send alice's draft.
  CHECK_FALSE(fx.draft(d.id, fx.bob));
  CHECK(api_error([&] { fx.update(d.id, d.version, DraftInput{}, false, fx.bob); }).status == 404);
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { delete_draft(tx, fx.bob, d.id); }); }).status == 404);
  CHECK(api_error([&] { fx.send(d, std::nullopt, fx.bob); }).status == 404);
  // ... nor attach alice's upload or reply to alice's message.
  const auto up = fx.upload("a.txt", "text/plain", false);
  DraftInput steal;
  steal.attachment_ids = std::vector<int64_t>{up.id};
  const ApiError e = api_error([&] { fx.create(steal, fx.bob); });
  CHECK(e.status == 400);
  CHECK(e.details.at("field").as_string() == "attachment_ids");
  const auto parent = parent_with_images(fx);
  DraftInput reply;
  reply.mode = DraftMode::Reply;
  reply.parent_message_id = std::optional<int64_t>(parent.message_id);
  CHECK(api_error([&] { fx.create(reply, fx.bob); }).status == 404);
}

TEST_CASE("drafts: stored-HTML cid invariant round trip", "[drafts][cid]") {
  SendFx fx;
  const auto img = fx.upload("paste.png", "image/png", /*inline=*/true);
  REQUIRE(img.content_id);
  REQUIRE(img.view_url);
  // The editor inserts <img data-att-id src=view_url>; attachment_ids omits the inline upload.
  DraftInput in;
  in.to = std::vector<Address>{kBob};
  in.html = "<p>截图：</p><img data-att-id=\"" + std::to_string(img.id) + "\" src=\"" + *img.view_url + "\">";
  const Draft d = fx.create(in);
  REQUIRE(d.attachments.size() == 1);
  CHECK(d.attachments[0].id == img.id);
  CHECK(d.attachments[0].is_inline);

  // Stored: cid only, no signed URL.
  const std::string stored = stored_html(fx, d.id);
  CHECK(stored.find("cid:" + *img.content_id) != std::string::npos);
  CHECK(stored.find("/api/files/") == std::string::npos);
  CHECK(stored.find("sig=") == std::string::npos);
  // Read: signed URL again, with data-att-id.
  CHECK(d.html.find("/api/files/" + std::to_string(img.id) + "?d=i") != std::string::npos);
  CHECK(d.html.find("data-att-id=\"" + std::to_string(img.id) + "\"") != std::string::npos);
  CHECK(d.html.find("cid:") == std::string::npos);

  // Saving what the client read back yields the same stored form (idempotent).
  DraftInput again;
  again.html = d.html;
  const Draft d2 = fx.update(d.id, d.version, again);
  CHECK(stored_html(fx, d.id) == stored);
  CHECK(d2.html == d.html);

  // A signed URL without data-att-id (e.g. pasted HTML) is rewritten too.
  DraftInput bare;
  bare.html = "<img src=\"" + *img.view_url + "\" alt=x>";
  fx.update(d.id, d2.version, bare);
  CHECK(stored_html(fx, d.id) == "<img src=\"cid:" + *img.content_id + "\" alt=x>");
  // Foreign URLs are untouched.
  DraftInput ext;
  ext.html = "<img src=\"https://cdn.example/x.png\">";
  fx.update(d.id, fx.draft(d.id)->version, ext);
  CHECK(stored_html(fx, d.id) == "<img src=\"https://cdn.example/x.png\">");
}

TEST_CASE("drafts: attachment_ids is the full list", "[drafts][attachments]") {
  SendFx fx;
  const auto a = fx.upload("a.pdf", "application/pdf", false);
  const auto b = fx.upload("b.pdf", "application/pdf", false);
  const auto inl = fx.upload("i.png", "image/png", true);
  DraftInput in;
  in.attachment_ids = std::vector<int64_t>{a.id, b.id};
  in.html = "<img src=\"cid:" + *inl.content_id + "\">";  // the inline upload is linked via its id below
  Draft d = fx.create(in);
  CHECK(d.attachments.size() == 2);

  DraftInput link_inline;
  link_inline.attachment_ids = std::vector<int64_t>{a.id, b.id, inl.id};
  d = fx.update(d.id, d.version, link_inline);
  CHECK(d.attachments.size() == 3);
  CHECK(fx.scalar("SELECT has_attachments FROM messages WHERE is_draft = 1") == 1);

  // Dropping b removes it; the inline image is still referenced by the body and stays.
  DraftInput drop;
  drop.attachment_ids = std::vector<int64_t>{a.id};
  d = fx.update(d.id, d.version, drop);
  std::vector<int64_t> ids;
  for (const auto& x : d.attachments) ids.push_back(x.id);
  CHECK(ids == std::vector<int64_t>{a.id, inl.id});
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM attachments WHERE id = ?", b.id) == 0);

  // Once the body no longer references it, an unlisted inline image goes too.
  DraftInput body;
  body.html = "<p>无图</p>";
  body.attachment_ids = std::vector<int64_t>{a.id};
  d = fx.update(d.id, d.version, body);
  CHECK(d.attachments.size() == 1);

  // Unknown ids, ids of other drafts and of sent mail are rejected.
  const Draft other = fx.create(DraftInput{});
  const auto c = fx.upload("c.pdf", "application/pdf", false);
  DraftInput on_other;
  on_other.attachment_ids = std::vector<int64_t>{c.id};
  fx.update(other.id, other.version, on_other);
  for (int64_t bad : {c.id, int64_t{424242}}) {
    DraftInput x;
    x.attachment_ids = std::vector<int64_t>{bad};
    const ApiError e = api_error([&] { fx.update(d.id, fx.draft(d.id)->version, x); });
    CHECK(e.code == "invalid_field");
  }
  // An empty list removes everything.
  DraftInput none;
  none.attachment_ids = std::vector<int64_t>{};
  CHECK(fx.update(d.id, fx.draft(d.id)->version, none).attachments.empty());
}

TEST_CASE("reply drafts: parent thread, identity, defaults, quote images copied", "[drafts][reply]") {
  SendFx fx;
  const auto parent = parent_with_images(fx, "support@team.example");
  const int64_t parent_img = parent.attachment_ids[0];

  // The frontend builds quoted_html from the cached parent: signed URLs of the PARENT's image.
  const std::string parent_img_url = fx.ts.urls.file_url(parent_img, fx.alice, 'i', fx.ts.urls.expiry(azm::now_ms()));
  DraftInput in;
  in.mode = DraftMode::Reply;
  in.parent_message_id = std::optional<int64_t>(parent.message_id);
  in.html = "<p>已处理</p>";
  in.quoted_html = std::optional<std::string>("<blockquote><p>见图</p><img data-att-id=\"" +
                                              std::to_string(parent_img) + "\" src=\"" + parent_img_url +
                                              "\"></blockquote>");
  const Draft d = fx.create(in);
  CHECK(d.thread_id == parent.thread_id);
  CHECK(d.mode == DraftMode::Reply);
  CHECK(d.parent_message_id == parent.message_id);
  CHECK(d.from_address_id == fx.support);    // delivered to support@ and alice may send as it
  CHECK(d.to == std::vector<Address>{kCustomer});  // default: the parent's sender
  CHECK(d.cc.empty());
  CHECK(d.subject == "Re: 订单问题");

  // Only the quote image is copied (same blob, same Content-ID); the PDF is not.
  REQUIRE(d.attachments.size() == 1);
  CHECK(d.attachments[0].content_id == "logo123");
  CHECK(d.attachments[0].is_inline);
  CHECK(d.attachments[0].id != parent_img);
  CHECK(fx.text_of("SELECT blob_sha256 FROM attachments WHERE id = ?", d.attachments[0].id) ==
        fx.text_of("SELECT blob_sha256 FROM attachments WHERE id = ?", parent_img));
  // quoted_html is stored with cid: and read back with the COPY's signed URL.
  CHECK(stored_quoted(fx, d.id).find("cid:logo123") != std::string::npos);
  CHECK(stored_quoted(fx, d.id).find("/api/files/") == std::string::npos);
  REQUIRE(d.quoted_html);
  CHECK(d.quoted_html->find("/api/files/" + std::to_string(d.attachments[0].id) + "?d=i") != std::string::npos);

  // Saving the quote again with the parent's URLs (cached parent) keeps the copy.
  DraftInput again;
  again.quoted_html = in.quoted_html;
  again.attachment_ids = std::vector<int64_t>{};  // client lists no attachments
  const Draft d2 = fx.update(d.id, d.version, again);
  CHECK(d2.attachments.size() == 1);
  CHECK(stored_quoted(fx, d.id).find("cid:logo123") != std::string::npos);

  // Thread view: the draft sits in the parent's thread.
  const auto t = fx.thread(parent.thread_id).value();
  CHECK(t.messages.size() == 2);
  CHECK(fx.folder_ids(Folder::Drafts) == std::vector<int64_t>{parent.thread_id});

  // bob (member without can_send_as) replying to his copy defaults to his own address.
  Msg bob_copy;
  bob_copy.owner = fx.bob;
  bob_copy.from = kCustomer;
  bob_copy.to = {kSupport};
  bob_copy.delivered_to = "support@team.example";
  bob_copy.message_id = "orig-1@customer.example";
  const auto bp = fx.ts.db.write([&](db::Tx& tx) { return test::mailfx::insert_message(tx, bob_copy); });
  DraftInput breply;
  breply.mode = DraftMode::Reply;
  breply.parent_message_id = std::optional<int64_t>(bp.message_id);
  CHECK(fx.create(breply, fx.bob).from_address_id == fx.bob_addr);
}

TEST_CASE("reply_all / forward initialization", "[drafts][reply]") {
  SendFx fx;
  const auto parent = parent_with_images(fx);
  DraftInput all;
  all.mode = DraftMode::ReplyAll;
  all.parent_message_id = std::optional<int64_t>(parent.message_id);
  const Draft ra = fx.create(all);
  // From + To (minus my identities: support@ is mine through the alias), Cc minus nothing mine.
  CHECK(ra.to == std::vector<Address>{kCustomer});
  CHECK(ra.cc == (std::vector<Address>{kCarol, kExternal}));
  CHECK(ra.from_address_id == fx.alice_addr);  // delivered to alice@ itself

  DraftInput fwd;
  fwd.mode = DraftMode::Forward;
  fwd.parent_message_id = std::optional<int64_t>(parent.message_id);
  const Draft f1 = fx.create(fwd);
  CHECK(f1.subject == "Fwd: 订单问题");
  CHECK(f1.to.empty());
  CHECK(f1.attachments.size() == 1);  // the quote image only
  fwd.include_parent_attachments = true;
  const Draft f2 = fx.create(fwd);
  REQUIRE(f2.attachments.size() == 2);
  CHECK(f2.attachments[1].filename == "合同.pdf");
  CHECK_FALSE(f2.attachments[1].is_inline);
  CHECK(f2.thread_id == parent.thread_id);

  // A reply mode needs a parent; a parent of another owner is not found.
  DraftInput bad;
  bad.mode = DraftMode::Reply;
  const ApiError e = api_error([&] { fx.create(bad); });
  CHECK(e.status == 400);
  CHECK(e.details.at("field").as_string() == "parent_message_id");
  bad.parent_message_id = std::optional<int64_t>(ra.id);  // a draft is not a parent
  CHECK(api_error([&] { fx.create(bad); }).status == 404);

  // Explicit fields win over the defaults (including an explicitly empty list).
  DraftInput expl;
  expl.mode = DraftMode::ReplyAll;
  expl.parent_message_id = std::optional<int64_t>(parent.message_id);
  expl.to = std::vector<Address>{kDave};
  expl.cc = std::vector<Address>{};
  expl.subject = "自定义";
  const Draft ex = fx.create(expl);
  CHECK(ex.to == std::vector<Address>{kDave});
  CHECK(ex.cc.empty());
  CHECK(ex.subject == "自定义");
}

TEST_CASE("delete_draft removes the message and an otherwise empty thread", "[drafts]") {
  SendFx fx;
  const Draft d = fx.simple_draft({kBob});
  const auto up = fx.upload("x.pdf", "application/pdf", false);
  DraftInput in;
  in.attachment_ids = std::vector<int64_t>{up.id};
  fx.update(d.id, d.version, in);
  fx.ts.notifier.clear();
  fx.ts.db.write([&](db::Tx& tx) { delete_draft(tx, fx.alice, d.id); });
  CHECK_FALSE(fx.draft(d.id));
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM threads WHERE id = ?", d.thread_id) == 0);
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM attachments WHERE id = ?", up.id) == 0);  // rows cascade, blob → GC
  CHECK(fx.scalar("SELECT COUNT(*) FROM message_fts") == 0);
  CHECK(fx.events_of(ws::events::kThreadsChanged, fx.alice).size() == 1);
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { delete_draft(tx, fx.alice, d.id); }); }).status == 404);

  // A reply draft leaves the parent's thread in place.
  const auto parent = parent_with_images(fx);
  DraftInput r;
  r.mode = DraftMode::Reply;
  r.parent_message_id = std::optional<int64_t>(parent.message_id);
  const Draft rd = fx.create(r);
  fx.ts.db.write([&](db::Tx& tx) { delete_draft(tx, fx.alice, rd.id); });
  const auto agg = fx.ts.db.read([&](db::Conn& c) { return test::mailfx::aggregates(c, parent.thread_id); });
  REQUIRE(agg);
  CHECK(agg->draft_count == 0);
  CHECK(agg->msg_count == 1);
  // Sent messages are not drafts.
  const auto res = fx.send(fx.simple_draft({kBob}));
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { delete_draft(tx, fx.alice, res.message_id); }); }).status ==
        404);
}

TEST_CASE("undo_send: inside the window restores the draft; race with the send job", "[drafts][undo]") {
  SendFx fx;
  fx.ts.db.write([&](db::Tx& tx) { tx.run("UPDATE user_settings SET undo_send_seconds = 10, signature_enabled = 1, "
                                          "signature_html = '<p>-- 张三</p>' WHERE user_id = ?", fx.alice); });
  DraftInput in;
  in.to = std::vector<Address>{kBob};
  in.subject = "撤回测试";
  in.html = "<p>正文</p>";
  in.quoted_html = std::optional<std::string>("<blockquote>旧</blockquote>");
  const Draft d = fx.create(in);
  const SendResult r = fx.send(d);
  CHECK(r.undo_ms == 10'000);
  CHECK(r.status == OutboundStatus::Queued);
  const auto job = fx.outbound(r.outbound_id).job_id;
  REQUIRE(job);
  CHECK(fx.folder_ids(Folder::Sent) == std::vector<int64_t>{r.thread_id});
  CHECK(fx.message(r.message_id)->outbound->undo_until == fx.outbound(r.outbound_id).send_after);

  fx.ts.notifier.clear();
  const Draft back = fx.ts.db.write([&](db::Tx& tx) { return undo_send(tx, fx.ts.urls, fx.alice, r.message_id); });
  CHECK(back.id == d.id);
  CHECK(back.version == d.version + 1);
  CHECK(back.html == "<p>正文</p>");  // the pre-freeze body: no signature, no merged quote
  CHECK(back.quoted_html == "<blockquote>旧</blockquote>");
  CHECK(back.subject == "撤回测试");
  CHECK(back.to == std::vector<Address>{kBob});
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Canceled);
  CHECK(fx.text_of("SELECT state FROM jobs WHERE id = ?", *job) == "canceled");
  CHECK(fx.scalar_of("SELECT COUNT(*) FROM delivery_events WHERE outbound_id = ? AND type = 'local.canceled'",
                     r.outbound_id) == 1);
  CHECK(fx.folder_ids(Folder::Sent).empty());
  CHECK(fx.folder_ids(Folder::Drafts) == std::vector<int64_t>{r.thread_id});
  CHECK_FALSE(fx.events_of(ws::events::kOutboundStatus, fx.alice).empty());
  // The send job, if it was already claimed, stops.
  CHECK_FALSE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r.outbound_id); }));

  // Undo → edit → resend: a new outbound row with a new uuid (B1).
  DraftInput edit;
  edit.html = "<p>修改后</p>";
  const Draft edited = fx.update(back.id, back.version, edit);
  const SendResult r2 = fx.send(edited);
  CHECK(r2.outbound_id != r.outbound_id);
  CHECK(fx.outbound(r2.outbound_id).uuid != fx.outbound(r.outbound_id).uuid);
  CHECK(fx.plan(r2.outbound_id).html.find("修改后") != std::string::npos);

  // Too late once the job moved it to sending.
  REQUIRE(fx.ts.db.write([&](db::Tx& tx) { return mark_sending(tx, r2.outbound_id); }));
  const ApiError late = api_error([&] { fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.alice, r2.message_id); }); });
  CHECK(late.status == 409);
  CHECK(late.code == "too_late");
  CHECK(fx.outbound(r2.outbound_id).status == OutboundStatus::Sending);
  // Already a draft again / other owner / inbound message.
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.alice, back.id + 1000); }); })
            .status == 404);
  CHECK(api_error([&] { fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.bob, r2.message_id); }); })
            .status == 404);
}

TEST_CASE("undo_send of a scheduled send is too late (cancel-schedule instead)", "[drafts][undo]") {
  SendFx fx;
  const SendResult r = fx.send(fx.simple_draft({kBob}), azm::now_ms() + 3'600'000);
  CHECK(r.undo_ms == 0);
  const ApiError e = api_error([&] { fx.ts.db.write([&](db::Tx& tx) { undo_send(tx, fx.ts.urls, fx.alice, r.message_id); }); });
  CHECK(e.code == "too_late");
  CHECK(fx.outbound(r.outbound_id).status == OutboundStatus::Queued);
}
