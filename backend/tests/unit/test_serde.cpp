// Owner: WP-B — JSON shapes (docs/API.md + Addendum B) and request parsers.
#include "core/errors.hpp"
#include "core/json.hpp"
#include "mail/serde.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <vector>

using namespace azm;
using namespace azm::mail;
namespace json = boost::json;

namespace {

// Golden comparison on parsed values (key order independent).
void check_json(const json::value& actual, std::string_view expected) {
  const json::value exp = json::parse(expected);
  INFO("actual:   " << json::serialize(actual));
  INFO("expected: " << json::serialize(exp));
  CHECK(actual == exp);
}

json::object obj(std::string_view s) { return json::parse(s).as_object(); }

std::string invalid_field_of(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const ApiError& e) {
    if (e.code != "invalid_field" || e.status != 400) return "code:" + e.code;
    if (const auto* f = e.details.if_contains("field"); f && f->is_string()) return std::string(f->as_string());
    return "no-field";
  }
  return "no-throw";
}

AttachmentView sample_attachment() {
  AttachmentView a;
  a.id = 9;
  a.filename = "报告.pdf";
  a.content_type = "application/pdf";
  a.size = 2048;
  a.is_inline = false;
  a.download_url = "https://api/x?d=a";
  a.view_url = "https://api/x?d=i";
  return a;
}

}  // namespace

TEST_CASE("serde: Address, Attachment", "[serde]") {
  check_json(to_json(Address{"张三", "z@x.cn"}), R"({"name":"张三","email":"z@x.cn"})");
  std::vector<Address> list{{"", "a@x.cn"}, {"B", "b@x.cn"}};
  check_json(to_json(std::span<const Address>(list)),
             R"([{"name":"","email":"a@x.cn"},{"name":"B","email":"b@x.cn"}])");
  check_json(to_json(sample_attachment()),
             R"({"id":9,"filename":"报告.pdf","content_type":"application/pdf","size":2048,"inline":false,
                 "content_id":null,"download_url":"https://api/x?d=a","view_url":"https://api/x?d=i"})");
  AttachmentView inl = sample_attachment();
  inl.is_inline = true;
  inl.content_id = "c@x";
  inl.view_url.reset();
  check_json(to_json(inl),
             R"({"id":9,"filename":"报告.pdf","content_type":"application/pdf","size":2048,"inline":true,
                 "content_id":"c@x","download_url":"https://api/x?d=a","view_url":null})");
}

TEST_CASE("serde: Message golden (inbound + outbound)", "[serde]") {
  MessageView m;
  m.id = 1;
  m.thread_id = 2;
  m.direction = Direction::In;
  m.from = {"Bob", "bob@ext.cn"};
  m.to = {{"Alice", "alice@team.cn"}};
  m.delivered_to = "support@team.cn";
  m.subject = "Hi";
  m.snippet = "hello";
  m.date = 1700000000000;
  m.html = "<p>hello</p>";
  m.attachments = {sample_attachment()};
  m.is_read = true;
  m.in_inbox = true;
  m.label_ids = {3, 4};
  m.auth = AuthResults{"pass", std::nullopt, "fail"};
  m.warnings = {"dmarc_fail"};
  m.message_id_header = "abc@ext.cn";
  m.raw_url = "https://api/raw";
  check_json(to_json(m), R"({
    "id":1,"thread_id":2,"direction":"in","is_draft":false,
    "from":{"name":"Bob","email":"bob@ext.cn"},"sent_by":null,
    "to":[{"name":"Alice","email":"alice@team.cn"}],"cc":[],"bcc":[],"reply_to":[],
    "delivered_to":"support@team.cn","subject":"Hi","snippet":"hello","date":1700000000000,
    "html":"<p>hello</p>","text":null,
    "attachments":[{"id":9,"filename":"报告.pdf","content_type":"application/pdf","size":2048,"inline":false,
                    "content_id":null,"download_url":"https://api/x?d=a","view_url":"https://api/x?d=i"}],
    "is_read":true,"is_starred":false,"in_inbox":true,"is_spam":false,"trashed":false,"label_ids":[3,4],
    "auth":{"spf":"pass","dkim":null,"dmarc":"fail"},"warnings":["dmarc_fail"],"outbound":null,
    "message_id_header":"abc@ext.cn","raw_url":"https://api/raw"})");

  MessageView o;
  o.id = 5;
  o.thread_id = 2;
  o.direction = Direction::Out;
  o.from = {"Support", "support@team.cn"};
  o.sent_by = Address{"Alice", "alice@team.cn"};
  o.date = 1;
  OutboundView ov;
  ov.id = 77;
  ov.status = OutboundStatus::DeliveryDelayed;
  ov.status_detail = "投递延迟";
  ov.scheduled_at = 2000;
  ov.scheduled_via = ScheduledVia::Local;
  ov.undo_until = std::nullopt;
  ov.sent_at = 1500;
  o.outbound = ov;
  check_json(to_json(o), R"({
    "id":5,"thread_id":2,"direction":"out","is_draft":false,
    "from":{"name":"Support","email":"support@team.cn"},"sent_by":{"name":"Alice","email":"alice@team.cn"},
    "to":[],"cc":[],"bcc":[],"reply_to":[],"delivered_to":null,"subject":"","snippet":"","date":1,
    "html":null,"text":null,"attachments":[],"is_read":false,"is_starred":false,"in_inbox":false,
    "is_spam":false,"trashed":false,"label_ids":[],"auth":null,"warnings":[],
    "outbound":{"id":77,"status":"delivery_delayed","status_detail":"投递延迟","scheduled_at":2000,
                "scheduled_via":"local","undo_until":null,"sent_at":1500},
    "message_id_header":null,"raw_url":null})");

  OutboundView q;
  q.id = 1;
  q.undo_until = 99;
  check_json(to_json(q), R"({"id":1,"status":"queued","status_detail":null,"scheduled_at":null,
                              "scheduled_via":null,"undo_until":99,"sent_at":null})");
}

TEST_CASE("serde: ThreadListItem / ThreadPage / ThreadDetail golden", "[serde]") {
  ThreadListItem t;
  t.id = 10;
  t.subject = "周报";
  t.snippet = "s";
  t.participants = {{"Me", "me@x.cn", true, false}, {"Bob", "b@x.cn", false, true}};
  t.message_count = 3;
  t.draft_count = 1;
  t.unread = true;
  t.starred = false;
  t.has_attachments = true;
  t.label_ids = {7};
  t.last_at = 123;
  t.in_inbox = true;
  t.latest_status = OutboundStatus::Delivered;
  t.scheduled_at = std::nullopt;
  t.attachments_preview = {{4, "a.pdf", "application/pdf"}};
  const char* item_json = R"({"id":10,"subject":"周报","snippet":"s",
    "participants":[{"name":"Me","email":"me@x.cn","is_me":true,"unread":false},
                    {"name":"Bob","email":"b@x.cn","is_me":false,"unread":true}],
    "message_count":3,"draft_count":1,"unread":true,"starred":false,"has_attachments":true,"label_ids":[7],
    "last_at":123,"in_inbox":true,"latest_status":"delivered","scheduled_at":null,
    "attachments_preview":[{"id":4,"filename":"a.pdf","content_type":"application/pdf"}]})";
  check_json(to_json(t), item_json);

  ThreadListItem empty;
  check_json(to_json(empty), R"({"id":0,"subject":"","snippet":"","participants":[],"message_count":0,
    "draft_count":0,"unread":false,"starred":false,"has_attachments":false,"label_ids":[],"last_at":0,
    "in_inbox":false,"latest_status":null,"scheduled_at":null,"attachments_preview":[]})");

  ThreadPage p;
  p.items = {t};
  p.next_cursor = "abc";
  p.total = 42;
  check_json(to_json(p), std::string(R"({"items":[)") + item_json + R"(],"next_cursor":"abc","total":42})");
  ThreadPage search;
  check_json(to_json(search), R"({"items":[],"next_cursor":null,"total":null})");

  ThreadDetail d;
  d.id = 10;
  d.subject = "x";
  d.label_ids = {1, 2};
  const json::object dj = to_json(d);
  check_json(dj, R"({"id":10,"subject":"x","label_ids":[1,2],"messages":[]})");
}

TEST_CASE("serde: Draft, SendResult, Counts, contacts, events, envelopes", "[serde]") {
  Draft d;
  d.id = 3;
  d.thread_id = 4;
  d.version = 2;
  d.from_address_id = 8;
  d.to = {{"", "a@x.cn"}};
  d.subject = "s";
  d.html = "<p>x</p>";
  d.updated_at = 5;
  const char* draft_json = R"({"id":3,"thread_id":4,"version":2,"mode":"new","parent_message_id":null,
    "from_address_id":8,"to":[{"name":"","email":"a@x.cn"}],"cc":[],"bcc":[],"subject":"s",
    "html":"<p>x</p>","quoted_html":null,"attachments":[],"updated_at":5})";
  check_json(to_json(d), draft_json);
  d.mode = DraftMode::ReplyAll;
  d.parent_message_id = 1;
  d.quoted_html = "<blockquote>q</blockquote>";
  const json::object dj = to_json(d);
  CHECK(dj.at("mode") == "reply_all");
  CHECK(dj.at("parent_message_id") == 1);
  CHECK(dj.at("quoted_html") == "<blockquote>q</blockquote>");
  check_json(draft_response(Draft{}), R"({"draft":{"id":0,"thread_id":0,"version":0,"mode":"new",
    "parent_message_id":null,"from_address_id":0,"to":[],"cc":[],"bcc":[],"subject":"","html":"",
    "quoted_html":null,"attachments":[],"updated_at":0}})");

  SendResult r;
  r.message_id = 1;
  r.thread_id = 2;
  r.outbound_id = 3;
  r.status = OutboundStatus::Queued;
  r.undo_ms = 5000;
  check_json(to_json(r), R"({"message_id":1,"thread_id":2,"outbound_id":3,"status":"queued","undo_ms":5000,
                              "scheduled_at":null})");
  r.scheduled_at = 99;
  r.undo_ms = 0;
  r.status = OutboundStatus::Scheduled;
  check_json(to_json(r), R"({"message_id":1,"thread_id":2,"outbound_id":3,"status":"scheduled","undo_ms":0,
                              "scheduled_at":99})");

  Counts c;
  c.inbox_unread = 1;
  c.drafts = 2;
  c.scheduled = 3;
  c.spam_unread = 4;
  c.labels[12] = {1, 5};
  c.labels[3] = {0, 0};
  check_json(to_json(c), R"({"inbox_unread":1,"drafts":2,"scheduled":3,"spam_unread":4,
                              "labels":{"12":{"unread":1,"total":5},"3":{"unread":0,"total":0}}})");
  check_json(to_json(Counts{}), R"({"inbox_unread":0,"drafts":0,"scheduled":0,"spam_unread":0,"labels":{}})");

  std::vector<ContactView> cs{{"张三", "z@x.cn", ContactKind::Team}, {"", "s@x.cn", ContactKind::Alias},
                              {"Ext", "e@y.cn", ContactKind::Contact}};
  check_json(contacts_response(cs), R"({"items":[{"name":"张三","email":"z@x.cn","kind":"team"},
    {"name":"","email":"s@x.cn","kind":"alias"},{"name":"Ext","email":"e@y.cn","kind":"contact"}]})");

  std::vector<DeliveryEventView> ev{{"email.bounced", 10, json::object{{"bounce", "550 no such user"}}},
                                    {"local.queued", 5, {}}};
  check_json(events_response(ev), R"({"events":[{"type":"email.bounced","occurred_at":10,
    "detail":{"bounce":"550 no such user"}},{"type":"local.queued","occurred_at":5,"detail":{}}]})");

  std::vector<int64_t> ids{1, 2, 3};
  check_json(thread_ids_response(ids), R"({"thread_ids":[1,2,3]})");
  check_json(thread_ids_response({}), R"({"thread_ids":[]})");
}

TEST_CASE("serde: parse_address / parse_address_list", "[serde]") {
  auto a = parse_address(json::parse(R"({"name":" 张三 ","email":" z@x.cn "})"), "to");
  CHECK(a.name == "张三");
  CHECK(a.email == "z@x.cn");
  CHECK(parse_address(json::parse(R"({"email":"a@b.cn"})"), "to").name.empty());
  CHECK(parse_address(json::parse(R"({"name":null,"email":"a@b.cn"})"), "to").name.empty());
  CHECK(invalid_field_of([] { parse_address(json::parse(R"({"email":"not-an-email"})"), "cc"); }) == "cc");
  CHECK(invalid_field_of([] { parse_address(json::parse(R"({"name":"x"})"), "cc"); }) == "cc");
  CHECK(invalid_field_of([] { parse_address(json::parse(R"("a@b.cn")"), "cc"); }) == "cc");
  CHECK(invalid_field_of([] { parse_address(json::parse(R"({"name":5,"email":"a@b.cn"})"), "cc"); }) == "cc");
  auto l = parse_address_list(json::parse(R"([{"email":"a@b.cn"},{"name":"B","email":"b@b.cn"}])"), "to");
  REQUIRE(l.size() == 2);
  CHECK(l[1].name == "B");
  CHECK(invalid_field_of([] { parse_address_list(json::parse(R"({"email":"a@b.cn"})"), "draft.to"); }) ==
        "draft.to");
}

TEST_CASE("serde: parse_draft_input absent vs null", "[serde]") {
  auto in = parse_draft_input(obj("{}"));
  CHECK_FALSE(in.mode);
  CHECK_FALSE(in.parent_message_id);
  CHECK_FALSE(in.to);
  CHECK_FALSE(in.quoted_html);
  CHECK_FALSE(in.attachment_ids);

  in = parse_draft_input(obj(R"({"mode":"reply","parent_message_id":12,"from_address_id":3,
    "to":[{"email":"a@b.cn"}],"cc":[],"bcc":null,"subject":"s","html":"<p>x</p>","quoted_html":"<q>",
    "attachment_ids":[1,2],"include_parent_attachments":true,"unknown":1})"));
  CHECK(in.mode == DraftMode::Reply);
  REQUIRE(in.parent_message_id);
  CHECK(*in.parent_message_id == 12);
  CHECK(in.from_address_id == 3);
  REQUIRE(in.to);
  CHECK(in.to->size() == 1);
  REQUIRE(in.cc);
  CHECK(in.cc->empty());
  CHECK_FALSE(in.bcc);  // null = absent for lists
  CHECK(in.subject == "s");
  CHECK(in.html == "<p>x</p>");
  REQUIRE(in.quoted_html);
  CHECK(*in.quoted_html == "<q>");
  CHECK(in.attachment_ids == std::vector<int64_t>{1, 2});
  CHECK(in.include_parent_attachments == true);

  in = parse_draft_input(obj(R"({"parent_message_id":null,"quoted_html":null})"));
  REQUIRE(in.parent_message_id);
  CHECK_FALSE(in.parent_message_id->has_value());  // present, explicitly null
  REQUIRE(in.quoted_html);
  CHECK_FALSE(in.quoted_html->has_value());

  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"mode":"bogus"})")); }) == "mode");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"mode":"reply"})"), "draft."); }) == "no-throw");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"subject":5})"), "draft."); }) == "draft.subject");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"attachment_ids":[1,"x"]})")); }) == "attachment_ids");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"parent_message_id":"1"})")); }) == "parent_message_id");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"quoted_html":1})")); }) == "quoted_html");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"to":[{"email":"bad"}]})"), "draft."); }) == "draft.to");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"include_parent_attachments":"yes"})")); }) ==
        "include_parent_attachments");
  CHECK(invalid_field_of([] { parse_draft_input(obj(R"({"from_address_id":1.5})")); }) == "from_address_id");
}

TEST_CASE("serde: draft update / send options / patch / actions / reschedule", "[serde]") {
  auto u = parse_draft_update(obj(R"({"version":3,"force":true,"subject":"x"})"));
  CHECK(u.version == 3);
  CHECK(u.force);
  CHECK(u.input.subject == "x");
  CHECK_FALSE(parse_draft_update(obj(R"({"version":1})")).force);
  CHECK(invalid_field_of([] { parse_draft_update(obj("{}")); }) == "version");
  CHECK(invalid_field_of([] { parse_draft_update(obj(R"({"version":"1"})")); }) == "version");

  auto s = parse_send_options(obj(R"({"version":2,"draft":{"subject":"final"},"scheduled_at":1800000000000})"));
  CHECK(s.version == 2);
  REQUIRE(s.draft);
  CHECK(s.draft->subject == "final");
  CHECK(s.scheduled_at == 1800000000000);
  CHECK(s.now_ms == 0);
  s = parse_send_options(obj(R"({"version":2,"scheduled_at":null})"));
  CHECK_FALSE(s.draft);
  CHECK_FALSE(s.scheduled_at);
  CHECK(invalid_field_of([] { parse_send_options(obj(R"({"draft":{}})")); }) == "version");
  CHECK(invalid_field_of([] { parse_send_options(obj(R"({"version":1,"draft":[]})")); }) == "draft");
  CHECK(invalid_field_of([] { parse_send_options(obj(R"({"version":1,"draft":{"to":5}})")); }) == "draft.to");
  CHECK(invalid_field_of([] { parse_send_options(obj(R"({"version":1,"scheduled_at":"soon"})")); }) ==
        "scheduled_at");

  auto p = parse_message_patch(obj(R"({"is_read":true,"add_label_ids":[1],"remove_label_ids":[2,3]})"));
  CHECK(p.is_read == true);
  CHECK_FALSE(p.is_starred);
  CHECK(p.add_label_ids == std::vector<int64_t>{1});
  CHECK(p.remove_label_ids == std::vector<int64_t>{2, 3});
  CHECK(parse_message_patch(obj("{}")).add_label_ids.empty());
  CHECK(invalid_field_of([] { parse_message_patch(obj(R"({"is_starred":1})")); }) == "is_starred");
  CHECK(invalid_field_of([] { parse_message_patch(obj(R"({"add_label_ids":"1"})")); }) == "add_label_ids");

  auto a = parse_thread_action_request(obj(R"({"thread_ids":[1,2],"action":"archive"})"));
  CHECK(a.thread_ids == std::vector<int64_t>{1, 2});
  CHECK(a.action == ThreadAction::Archive);
  CHECK_FALSE(a.label_id);
  a = parse_thread_action_request(obj(R"({"thread_ids":[],"action":"add_label","label_id":5})"));
  CHECK(a.action == ThreadAction::AddLabel);
  CHECK(a.label_id == 5);
  CHECK(invalid_field_of([] { parse_thread_action_request(obj(R"({"action":"archive"})")); }) == "thread_ids");
  CHECK(invalid_field_of([] { parse_thread_action_request(obj(R"({"thread_ids":[1]})")); }) == "action");
  CHECK(invalid_field_of([] { parse_thread_action_request(obj(R"({"thread_ids":[1],"action":"move"})")); }) ==
        "action");
  CHECK(invalid_field_of([] { parse_thread_action_request(obj(R"({"thread_ids":[1],"action":"remove_label"})")); }) ==
        "label_id");
  CHECK(invalid_field_of([] { parse_thread_action_request(obj(R"({"thread_ids":["1"],"action":"read"})")); }) ==
        "thread_ids");
  json::array many;
  for (int i = 0; i < 1001; ++i) many.emplace_back(i);
  json::object big{{"thread_ids", many}, {"action", "read"}};
  CHECK(invalid_field_of([&] { parse_thread_action_request(big); }) == "thread_ids");

  CHECK(parse_reschedule(obj(R"({"scheduled_at":1800000000000})")) == 1800000000000);
  CHECK(invalid_field_of([] { parse_reschedule(obj("{}")); }) == "scheduled_at");
  CHECK(invalid_field_of([] { parse_reschedule(obj(R"({"scheduled_at":null})")); }) == "scheduled_at");
}
