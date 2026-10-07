// Owner: WP-D — api/dto golden JSON (docs/API.md + Addendum B shapes) and request parsers.
#include "test_support.hpp"

#include "api/dto.hpp"
#include "core/errors.hpp"
#include "core/json.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace azm;
namespace json = boost::json;

namespace {

// Object equality in boost::json ignores key order, and an extra or missing key fails.
void check_json(const json::value& actual, std::string_view expected) {
  const json::value want = json::parse(expected);
  INFO("actual:   " << json::serialize(actual));
  INFO("expected: " << json::serialize(want));
  CHECK(actual == want);
}

template <class F>
ApiError api_error(F&& f) {
  try {
    f();
  } catch (const ApiError& e) {
    return e;
  }
  FAIL("expected ApiError");
  return ApiError(0, "", "");
}

std::string field_of(const ApiError& e) {
  const auto* f = e.details.if_contains("field");
  return f && f->is_string() ? std::string(f->as_string()) : std::string();
}

void check_invalid_field(const std::function<void()>& f, std::string_view field) {
  const ApiError e = api_error(f);
  CHECK(e.status == 400u);
  CHECK(e.code == "invalid_field");
  CHECK(field_of(e) == field);
}

json::object obj(std::string_view text) { return json::parse(text).as_object(); }

// Minimal BlobStore that only reports origins (server_info never touches blobs).
struct OriginsStore final : BlobStore {
  std::string_view kind() const override { return "r2"; }
  std::filesystem::path tmp_dir() const override { return {}; }
  BlobRef put_file(const std::filesystem::path&, std::optional<std::string>) override { throw BlobError("x"); }
  BlobRef put_bytes(std::string_view) override { throw BlobError("x"); }
  void get_to_file(std::string_view, const std::filesystem::path&) override { throw BlobError("x"); }
  std::string get_bytes(std::string_view, std::size_t) override { throw BlobError("x"); }
  bool exists(std::string_view) override { return false; }
  void remove(std::string_view) override {}
  ServePlan serve(std::string_view, const ServeOptions&) override { throw BlobError("x"); }
  std::vector<std::string> public_origins() const override {
    return {"https://ACCT.r2.cloudflarestorage.com/", "http://127.0.0.1:8080/x"};
  }
};

repo::User sample_user() {
  repo::User u;
  u.id = 7;
  u.email = "alice@team.example";
  u.display_name = "Alice";
  u.password_hash = "$scrypt$secret";
  u.is_admin = true;
  u.created_at = 100;
  u.updated_at = 200;
  return u;
}

}  // namespace

// ---- output ----------------------------------------------------------------------------------

TEST_CASE("dto: Settings, Identity, Label", "[dto]") {
  repo::UserSettings s;
  s.undo_send_seconds = 10;
  s.signature_html = "<b>sig</b>";
  s.signature_enabled = false;
  s.timezone = "Asia/Shanghai";
  s.page_size = 25;
  s.remote_images = repo::RemoteImages::Always;
  s.trusted_image_senders = {"a@x.example", "b@y.example"};
  s.display_name = "张三";
  check_json(api::to_json(s), R"({"undo_send_seconds":10,"signature_html":"<b>sig</b>",
    "signature_enabled":false,"timezone":"Asia/Shanghai","page_size":25,"remote_images":"always",
    "trusted_image_senders":["a@x.example","b@y.example"],"display_name":"张三"})");
  check_json(api::to_json(repo::UserSettings{}), R"({"undo_send_seconds":5,"signature_html":"",
    "signature_enabled":true,"timezone":"Asia/Shanghai","page_size":50,"remote_images":"ask",
    "trusted_image_senders":[],"display_name":""})");

  repo::Identity i{3, "support@team.example", "Support", repo::AddressKind::Alias, false};
  check_json(api::to_json(i), R"({"address_id":3,"email":"support@team.example",
    "display_name":"Support","kind":"alias","is_default":false})");

  repo::Label l{9, 7, "工作", "#aabbcc", 2, 123};
  check_json(api::to_json(l), R"({"id":9,"name":"工作","color":"#aabbcc","sort_order":2})");
  const std::vector<repo::Label> labels{l, l};
  const auto arr = api::to_json_array<repo::Label>(labels);
  REQUIRE(arr.size() == 2);
  CHECK(arr[1].as_object().at("id") == 9);
  CHECK(api::to_json_array<repo::Label>(std::vector<repo::Label>{}).empty());
}

TEST_CASE("dto: Me, login response, health, server_info", "[dto]") {
  const std::vector<repo::Identity> ids{{11, "alice@team.example", "Alice", repo::AddressKind::User, true}};
  api::ServerInfo srv{{"https://api.example", "https://acct.r2.cloudflarestorage.com"}, "r2", "1.2.3"};
  const auto me = api::me_json(sample_user(), repo::UserSettings{}, ids, srv);
  check_json(me, R"({"id":7,"email":"alice@team.example","display_name":"Alice","is_admin":true,
    "settings":{"undo_send_seconds":5,"signature_html":"","signature_enabled":true,
      "timezone":"Asia/Shanghai","page_size":50,"remote_images":"ask","trusted_image_senders":[],
      "display_name":""},
    "identities":[{"address_id":11,"email":"alice@team.example","display_name":"Alice","kind":"user","is_default":true}],
    "server":{"files_origins":["https://api.example","https://acct.r2.cloudflarestorage.com"],
      "blob_backend":"r2","version":"1.2.3"}})");
  // Never the password hash.
  CHECK(json::serialize(me).find("scrypt") == std::string::npos);

  check_json(api::login_response("tok", 99, json::object{{"id", 7}}),
             R"({"token":"tok","expires_at":99,"user":{"id":7}})");
  check_json(api::health_json("ok", "0.1.0", "ok", 5), R"({"status":"ok","version":"0.1.0","db":"ok","time":5})");

  test::TestServices ts;
  ts.cfg.public_api_base_url = "HTTPS://Mail-API.Example.com:8443/base/";
  auto info = api::server_info(ts.svc);
  CHECK(info.files_origins == std::vector<std::string>{"https://mail-api.example.com:8443"});
  CHECK(info.blob_backend == "local");
  CHECK(info.version == AZMAIL_VERSION);
  ts.cfg.public_api_base_url = "http://127.0.0.1:8080";
  CHECK(api::server_info(ts.svc).files_origins == std::vector<std::string>{"http://127.0.0.1:8080"});

  // Mixed store (Addendum A): the secondary store's origins are listed too, normalized, deduped.
  OriginsStore r2;
  ts.svc.secondary_blobs = &r2;
  info = api::server_info(ts.svc);
  CHECK(info.files_origins == std::vector<std::string>{"http://127.0.0.1:8080",
                                                       "https://acct.r2.cloudflarestorage.com"});
  CHECK(info.blob_backend == "local");  // the primary store
}

TEST_CASE("dto: AdminUser and AdminAlias", "[dto]") {
  repo::AdminUserRow r;
  r.user = sample_user();
  r.user.last_login_at = 555;
  r.message_count = 3;
  r.storage_bytes = 4096;
  r.aliases = {{5, "support@team.example", true}};
  check_json(api::to_json(r), R"({"id":7,"email":"alice@team.example","display_name":"Alice",
    "is_admin":true,"disabled":false,"created_at":100,"last_login_at":555,"message_count":3,
    "storage_bytes":4096,"aliases":[{"id":5,"email":"support@team.example","can_send_as":true}]})");
  r.user.last_login_at.reset();
  r.aliases.clear();
  CHECK(api::to_json(r).at("last_login_at").is_null());
  CHECK(api::to_json(r).at("aliases").as_array().empty());

  repo::Alias a;
  a.id = 5;
  a.email = "support@team.example";
  a.display_name = "客服";
  a.share_sent = false;
  a.created_at = 10;
  a.members = {{7, "alice@team.example", "Alice", true}, {8, "bob@team.example", "", false}};
  check_json(api::to_json(a), R"({"id":5,"email":"support@team.example","display_name":"客服",
    "share_sent":false,"created_at":10,"members":[
      {"user_id":7,"email":"alice@team.example","display_name":"Alice","can_send_as":true},
      {"user_id":8,"email":"bob@team.example","display_name":"","can_send_as":false}]})");
}

TEST_CASE("dto: DomainRow and DomainStatus", "[dto]") {
  repo::Domain d{4, "team.example", true, 77};
  check_json(api::to_json(d), R"({"id":4,"name":"team.example","receiving_enabled":true,"created_at":77})");
  check_json(api::domain_status_json(d, std::nullopt), R"({"id":4,"name":"team.example","resend":null})");

  resend::DomainInfo info;
  info.id = "dom_1";
  info.name = "team.example";
  info.status = "verified";
  info.region = "us-east-1";
  info.created_at_ms = 1000;
  info.records = {{"MX", "team.example", "MX", "Auto", "verified", "feedback-smtp.example", 10},
                  {"DKIM", "resend._domainkey", "TXT", "Auto", "pending", "p=MIGf", std::nullopt}};
  check_json(api::domain_status_json(d, info), R"({"id":4,"name":"team.example","resend":{
    "id":"dom_1","status":"verified","region":"us-east-1","created_at":1000,"records":[
      {"record":"MX","name":"team.example","type":"MX","ttl":"Auto","status":"verified","value":"feedback-smtp.example","priority":10},
      {"record":"DKIM","name":"resend._domainkey","type":"TXT","ttl":"Auto","status":"pending","value":"p=MIGf"}]}})");
  info.region.reset();
  info.created_at_ms.reset();
  info.records.clear();
  check_json(api::domain_status_json(d, info), R"({"id":4,"name":"team.example","resend":{
    "id":"dom_1","status":"verified","region":null,"created_at":null,"records":[]}})");
}

TEST_CASE("dto: webhook events, inbound, outbox, jobs", "[dto]") {
  repo::WebhookEventRow e{1, "msg_1", "email.delivered", std::nullopt, 10, std::nullopt, std::nullopt};
  check_json(api::to_json(e), R"({"id":1,"svix_id":"msg_1","type":"email.delivered",
    "resend_email_id":null,"received_at":10,"processed_at":null,"result":null})");
  repo::WebhookEventPage page;
  page.items = {e};
  page.items.push_back({2, "msg_2", "email.received", "re_2", 11, 12, "enqueued"});
  page.next_cursor = "2";
  check_json(api::to_json(page), R"({"items":[
    {"id":1,"svix_id":"msg_1","type":"email.delivered","resend_email_id":null,"received_at":10,"processed_at":null,"result":null},
    {"id":2,"svix_id":"msg_2","type":"email.received","resend_email_id":"re_2","received_at":11,"processed_at":12,"result":"enqueued"}],
    "next_cursor":"2"})");
  check_json(api::to_json(repo::WebhookEventPage{}), R"({"items":[],"next_cursor":null})");

  repo::InboundRow in;
  in.id = 3;
  in.resend_id = "re_3";
  in.state = "unroutable";
  in.source = "poll";
  in.recipients = {"ghost@team.example"};
  in.created_at = 1;
  in.updated_at = 2;
  check_json(api::to_json(in), R"({"id":3,"resend_id":"re_3","state":"unroutable","source":"poll",
    "message_id_header":null,"from_email":null,"subject":null,"received_at":null,
    "recipients":["ghost@team.example"],"error":null,"created_at":1,"updated_at":2})");
  in.message_id_header = "abc@x";
  in.from_email = "x@ext.example";
  in.subject = "Hi";
  in.received_at = 5;
  in.error = "boom";
  const auto j = api::to_json(in);
  CHECK(j.at("message_id_header") == "abc@x");
  CHECK(j.at("received_at") == 5);
  CHECK(j.at("error") == "boom");

  repo::OutboxRow o;
  o.id = 4;
  o.uuid = "u-4";
  o.sender_user_id = 7;
  o.sender_email = "alice@team.example";
  o.from_email = "support@team.example";
  o.status = "failed";
  o.status_detail = "发送配额已用完";
  o.error_name = "daily_quota_exceeded";
  o.scheduled_at = 9;
  o.scheduled_via = "local";
  o.total_bytes = 10;
  o.created_at = 1;
  o.updated_at = 2;
  check_json(api::to_json(o), R"({"id":4,"uuid":"u-4","sender_user_id":7,"sender_email":"alice@team.example",
    "from_email":"support@team.example","status":"failed","status_detail":"发送配额已用完",
    "error_name":"daily_quota_exceeded","scheduled_at":9,"scheduled_via":"local","resend_id":null,
    "total_bytes":10,"last_event":null,"last_event_at":null,"created_at":1,"updated_at":2})");

  repo::JobRow jr;
  jr.id = 5;
  jr.kind = "inbound.fetch";
  jr.lane = "inbound";
  jr.priority = 50;
  jr.payload = obj(R"({"resend_id":"re_1","source":"poll"})");
  jr.state = "dead";
  jr.run_at = 3;
  jr.attempts = 8;
  jr.max_attempts = 8;
  jr.dedupe_key = "in:re_1";
  jr.last_error = "404";
  jr.created_at = 1;
  jr.updated_at = 2;
  check_json(api::to_json(jr), R"({"id":5,"kind":"inbound.fetch","lane":"inbound","priority":50,
    "payload":{"resend_id":"re_1","source":"poll"},"state":"dead","run_at":3,"attempts":8,
    "max_attempts":8,"locked_until":null,"dedupe_key":"in:re_1","last_error":"404","created_at":1,
    "updated_at":2})");
  CHECK(api::to_json(repo::JobRow{}).at("payload").is_object());  // {} when empty, never a string
}

TEST_CASE("dto: AdminStats", "[dto]") {
  repo::AdminStats s;
  s.users = 2;
  s.messages = 30;
  s.storage_bytes = 1024;
  s.queue_pending = 3;
  s.queue_dead = 1;
  s.sent_24h = 4;
  s.received_24h = 5;
  s.failed_24h = 6;
  s.last_poll_at = 77;
  s.quota_blocked = true;
  s.storage = {"r2", "proxy", 12, 1024};
  check_json(api::to_json(s), R"({"users":2,"messages":30,"storage_bytes":1024,
    "queue":{"pending":3,"dead":1},"sent_24h":4,"received_24h":5,"failed_24h":6,
    "last_webhook_at":null,"last_poll_at":77,"quota_blocked":true,
    "storage":{"backend":"r2","delivery":"proxy","blob_count":12,"blob_bytes":1024}})");
}

// ---- parsers ---------------------------------------------------------------------------------

TEST_CASE("dto: login / password parsing and the password policy", "[dto][parse]") {
  auto l = api::parse_login(obj(R"({"email":"A@b.example","password":"pw","extra":1})"));
  CHECK(l.email == "A@b.example");
  CHECK(l.password == "pw");
  check_invalid_field([] { api::parse_login(obj(R"({"password":"x"})")); }, "email");
  check_invalid_field([] { api::parse_login(obj(R"({"email":"a@b.example","password":5})")); }, "password");
  check_invalid_field([] { api::parse_login(obj(R"({"email":null,"password":"x"})")); }, "email");

  auto p = api::parse_password_change(obj(R"({"current_password":"old","new_password":"newnewnew"})"));
  CHECK(p.current_password == "old");
  CHECK(p.new_password == "newnewnew");
  check_invalid_field([] { api::parse_password_change(obj(R"({"current_password":"x"})")); }, "new_password");

  auto weak = [](std::string pw) {
    const ApiError e = api_error([&] { api::validate_new_password(pw); });
    CHECK(e.status == 422u);
    CHECK(e.code == "weak_password");
  };
  weak("");
  weak("1234567");
  weak(std::string(257, 'a'));
  weak("        ");
  weak("\t\t\t\t\n\n\n\n");
  CHECK_NOTHROW(api::validate_new_password("12345678"));
  CHECK_NOTHROW(api::validate_new_password(std::string(256, 'a')));
  CHECK_NOTHROW(api::validate_new_password("密码很安全"));  // 15 bytes UTF-8
}

TEST_CASE("dto: settings patch parsing", "[dto][parse]") {
  const auto p = api::parse_settings_patch(obj(R"({"undo_send_seconds":20,"signature_html":"<p>x</p>",
    "signature_enabled":false,"timezone":"UTC","page_size":30,"remote_images":"always",
    "trusted_image_senders":["a@b.example"],"display_name":"Bob"})"));
  CHECK(p.undo_send_seconds == 20);
  CHECK(p.signature_html == "<p>x</p>");
  CHECK(p.signature_enabled == false);
  CHECK(p.timezone == "UTC");
  CHECK(p.page_size == 30);
  CHECK(p.remote_images == repo::RemoteImages::Always);
  CHECK(p.trusted_image_senders == std::vector<std::string>{"a@b.example"});
  CHECK(p.display_name == "Bob");

  const auto empty = api::parse_settings_patch(obj(R"({"page_size":null,"unknown":true})"));
  CHECK_FALSE(empty.page_size);
  CHECK_FALSE(empty.undo_send_seconds);
  CHECK_FALSE(empty.remote_images);
  CHECK(api::parse_settings_patch(obj(R"({"remote_images":"ask"})")).remote_images == repo::RemoteImages::Ask);
  CHECK(api::parse_settings_patch(obj(R"({"page_size":20.0})")).page_size == 20);

  check_invalid_field([] { api::parse_settings_patch(obj(R"({"undo_send_seconds":"5"})")); }, "undo_send_seconds");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"page_size":10.5})")); }, "page_size");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"page_size":99999999999})")); }, "page_size");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"remote_images":"never"})")); }, "remote_images");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"signature_enabled":1})")); }, "signature_enabled");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"trusted_image_senders":[1]})")); },
                      "trusted_image_senders");
  check_invalid_field([] { api::parse_settings_patch(obj(R"({"trusted_image_senders":"a@b"})")); },
                      "trusted_image_senders");
}

TEST_CASE("dto: label parsing", "[dto][parse]") {
  const auto in = api::parse_label_input(obj(R"({"name":"Work","color":"#112233","sort_order":4})"));
  CHECK(in.name == "Work");
  CHECK(in.color == "#112233");
  CHECK(in.sort_order == 4);
  CHECK_FALSE(api::parse_label_input(obj(R"({"name":"W","color":"#000000"})")).sort_order);
  check_invalid_field([] { api::parse_label_input(obj(R"({"color":"#000000"})")); }, "name");
  check_invalid_field([] { api::parse_label_input(obj(R"({"name":"W"})")); }, "color");
  check_invalid_field([] { api::parse_label_input(obj(R"({"name":"W","color":"#000000","sort_order":"1"})")); },
                      "sort_order");

  const auto p = api::parse_label_patch(obj(R"({"color":"#ffffff"})"));
  CHECK_FALSE(p.name);
  CHECK(p.color == "#ffffff");
  CHECK_FALSE(p.sort_order);
  check_invalid_field([] { api::parse_label_patch(obj(R"({"name":3})")); }, "name");
}

TEST_CASE("dto: admin user / alias / domain parsing", "[dto][parse]") {
  const auto c = api::parse_admin_user_create(obj(R"({"email":"a@b.example","display_name":"A","password":"pw12345678","is_admin":true})"));
  CHECK(c.email == "a@b.example");
  CHECK(c.display_name == "A");
  CHECK(c.password == "pw12345678");
  CHECK(c.is_admin);
  const auto c2 = api::parse_admin_user_create(obj(R"({"email":"a@b.example","password":"pw"})"));
  CHECK(c2.display_name.empty());
  CHECK_FALSE(c2.is_admin);
  check_invalid_field([] { api::parse_admin_user_create(obj(R"({"email":"a@b.example"})")); }, "password");
  check_invalid_field([] { api::parse_admin_user_create(obj(R"({"email":"a@b.example","password":"x","is_admin":"yes"})")); },
                      "is_admin");

  const auto up = api::parse_admin_user_patch(obj(R"({"disabled":true,"password":"newpassword"})"));
  CHECK_FALSE(up.display_name);
  CHECK_FALSE(up.is_admin);
  CHECK(up.disabled == true);
  CHECK(up.password == "newpassword");
  check_invalid_field([] { api::parse_admin_user_patch(obj(R"({"disabled":"no"})")); }, "disabled");

  const auto a = api::parse_alias_input(obj(R"({"email":"s@b.example","display_name":"S","share_sent":false,
    "members":[{"user_id":1,"can_send_as":true},{"user_id":2}]})"));
  CHECK(a.email == "s@b.example");
  CHECK_FALSE(a.share_sent);
  REQUIRE(a.members.size() == 2);
  CHECK(a.members[0].user_id == 1);
  CHECK(a.members[0].can_send_as);
  CHECK_FALSE(a.members[1].can_send_as);
  const auto a2 = api::parse_alias_input(obj(R"({"email":"s@b.example"})"));
  CHECK(a2.share_sent);
  CHECK(a2.members.empty());
  CHECK(a2.display_name.empty());
  for (const char* bad : {R"({"email":"s@b","members":[1]})", R"({"email":"s@b","members":[{"user_id":"1"}]})",
                          R"({"email":"s@b","members":[{"user_id":0}]})",
                          R"({"email":"s@b","members":[{"user_id":1,"can_send_as":1}]})",
                          R"({"email":"s@b","members":{"user_id":1}})"}) {
    INFO(bad);
    check_invalid_field([&] { api::parse_alias_input(obj(bad)); }, "members");
  }

  const auto ap = api::parse_alias_patch(obj(R"({"members":[]})"));
  REQUIRE(ap.members);
  CHECK(ap.members->empty());
  CHECK_FALSE(ap.email);
  CHECK_FALSE(api::parse_alias_patch(obj(R"({"members":null})")).members);
  CHECK(api::parse_alias_patch(obj(R"({"share_sent":true})")).share_sent == true);

  CHECK(api::parse_domain_input(obj(R"({"name":"x.example"})")) == "x.example");
  check_invalid_field([] { api::parse_domain_input(obj("{}")); }, "name");
}
