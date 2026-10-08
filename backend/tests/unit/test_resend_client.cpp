// Owner: WP-C — resend::Client wire format, error classification and rate limiting against a
// scripted fake transport, plus one end-to-end exchange over real HTTP (fake server).
#include "../fakes/fake_http_server.hpp"
#include "config.hpp"
#include "core/errors.hpp"
#include "core/time.hpp"
#include "net/http_client.hpp"
#include "resend/client.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

using namespace azm;
using namespace azm::resend;
using Catch::Matchers::ContainsSubstring;
namespace bhttp = boost::beast::http;
namespace json = boost::json;

namespace {

// Records requests; answers from a queue (default 200 {}).
struct ScriptedHttp final : net::HttpClient {
  std::vector<net::HttpRequest> seen;
  std::deque<net::HttpResponse> replies;
  std::optional<net::NetError> throw_next;

  net::HttpResponse send(const net::HttpRequest& req) override {
    seen.push_back(req);
    if (throw_next) {
      auto e = *throw_next;
      throw_next.reset();
      throw e;
    }
    if (replies.empty()) return reply(200, "{}");
    auto r = replies.front();
    replies.pop_front();
    r.final_url = req.url;
    return r;
  }
  static net::HttpResponse reply(unsigned status, std::string body,
                                 std::vector<std::pair<std::string, std::string>> headers = {}) {
    net::HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    r.headers = std::move(headers);
    return r;
  }
  void push(unsigned status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) {
    replies.push_back(reply(status, std::move(body), std::move(headers)));
  }
  std::optional<std::string> header(std::size_t i, std::string_view name) const {
    for (const auto& [k, v] : seen.at(i).headers)
      if (iequals(k, name)) return v;
    return std::nullopt;
  }
};

Config test_config() {
  Config cfg;
  cfg.resend_api_base = "https://api.resend.test/";  // trailing slash is trimmed
  cfg.resend_api_key = "re_test_key";
  cfg.resend_user_agent = "azmail/0.1.0";
  cfg.resend_timeout_sec = 7;
  return cfg;
}

struct Fixture {
  Config cfg = test_config();
  ManualClock clock{1'700'000'000'000};
  RateLimiter limiter{{.rps = 100, .burst = 100, .low_reserve = 2}, clock};
  ScriptedHttp http;
  Client client{cfg, http, limiter};
};

Error capture(const std::function<void()>& f) {
  try {
    f();
  } catch (const Error& e) {
    return e;
  }
  FAIL("expected resend::Error");
  return {};
}

}  // namespace

TEST_CASE("resend: classify_error by status and name", "[resend]") {
  using K = Error::Kind;
  auto c = [](int s, std::string_view body, std::optional<std::string_view> ra = std::nullopt) {
    return classify_error(s, body, ra);
  };
  auto e = c(429, R"({"statusCode":429,"name":"rate_limit_exceeded","message":"Too many requests"})", "3");
  CHECK(e.kind == K::RateLimited);
  CHECK(e.retry_after == std::chrono::seconds(3));
  CHECK(e.message == "Too many requests");
  CHECK(e.http_status == 429);
  CHECK(e.retryable());
  CHECK(c(429, R"({"name":"rate_limit_exceeded"})").retry_after == std::chrono::seconds(1));  // default
  CHECK(c(429, R"({"name":"rate_limit_exceeded"})", "0.2").retry_after == std::chrono::seconds(1));
  CHECK(c(429, R"({"name":"rate_limit_exceeded"})", "soon").retry_after == std::chrono::seconds(1));
  CHECK(c(429, "not json").kind == K::RateLimited);
  CHECK(c(429, R"({"name":"daily_quota_exceeded"})").kind == K::Quota);
  CHECK(c(429, R"({"name":"monthly_quota_exceeded"})").kind == K::Quota);
  CHECK_FALSE(c(429, R"({"name":"daily_quota_exceeded"})").retryable());
  CHECK(c(422, R"({"name":"validation_error","message":"Invalid `to` field"})").kind == K::Validation);
  CHECK(c(400, R"({"name":"invalid_idempotency_key"})").kind == K::Validation);
  CHECK(c(422, R"({"name":"missing_required_field"})").kind == K::Validation);
  CHECK(c(403, R"({"name":"validation_error","message":"testing emails only"})").kind == K::Validation);
  CHECK(c(400, R"({"name":"something_else"})").kind == K::Validation);
  CHECK(c(401, R"({"name":"missing_api_key"})").kind == K::Auth);
  CHECK(c(403, R"({"name":"invalid_api_key"})").kind == K::Auth);
  CHECK(c(401, "").kind == K::Auth);
  CHECK(c(404, R"({"name":"not_found"})").kind == K::NotFound);
  CHECK(c(409, R"({"name":"invalid_idempotent_request"})").kind == K::IdempotencyConflict);
  CHECK(c(409, R"({"name":"concurrent_idempotent_requests"})").kind == K::IdempotencyInFlight);
  CHECK(c(409, R"({"name":"concurrent_idempotent_requests"})").retryable());
  CHECK(c(409, "{}").kind == K::IdempotencyConflict);
  CHECK(c(500, R"({"name":"internal_server_error"})").kind == K::Server);
  CHECK(c(502, "<html>bad gateway</html>").kind == K::Server);
  CHECK(c(503, "").retryable());

  const auto cf = c(403, "error code: 1010");
  CHECK(cf.kind == K::Auth);
  CHECK(cf.name == "cloudflare_1010");
  CHECK_THAT(cf.message, ContainsSubstring("User-Agent"));
  CHECK_FALSE(cf.retryable());
}

TEST_CASE("resend: parse_webhook envelope", "[resend][webhook]") {
  const auto env = parse_webhook(R"({
    "type":"email.bounced","created_at":"2026-10-07 12:00:00.123456+00",
    "data":{"email_id":"re_1","message_id":"<m@x>","to":["a@b","c@d"],"from":"Me <me@x>",
            "subject":"Hi","tags":{"azmail_outbound":"uuid-1","n":5},
            "bounce":{"message":"mailbox full"}}})");
  CHECK(env.type == "email.bounced");
  CHECK(env.created_at_ms == *utc_ms(2026, 10, 7, 12, 0, 0, 123));
  CHECK(env.email_id == std::optional<std::string>("re_1"));
  CHECK(env.message_id == std::optional<std::string>("<m@x>"));
  CHECK(env.to == std::vector<std::string>{"a@b", "c@d"});
  CHECK(env.from == std::optional<std::string>("Me <me@x>"));
  CHECK(env.subject == std::optional<std::string>("Hi"));
  CHECK(env.tags.at("azmail_outbound") == "uuid-1");
  CHECK(env.tags.at("n") == "5");
  CHECK(env.data.contains("bounce"));
  CHECK(env.is_outbound_event());

  const auto arr = parse_webhook(
      R"({"type":"email.sent","data":{"email_id":"re_2","to":"x@y","tags":[{"name":"azmail_outbound","value":"u2"},{"bad":1}]}})");
  CHECK(arr.tags.at("azmail_outbound") == "u2");
  CHECK(arr.tags.size() == 1);
  CHECK(arr.to == std::vector<std::string>{"x@y"});
  CHECK(arr.created_at_ms == 0);

  const auto rcv = parse_webhook(R"({"type":"email.received","created_at":"bogus","data":{"email_id":"in_1"}})");
  CHECK(rcv.is_received());
  CHECK(rcv.created_at_ms == 0);
  CHECK(rcv.tags.empty());

  CHECK_THROWS_AS(parse_webhook("not json"), std::invalid_argument);
  CHECK_THROWS_AS(parse_webhook("[]"), std::invalid_argument);
  CHECK_THROWS_AS(parse_webhook(R"({"data":{}})"), std::invalid_argument);
  CHECK_THROWS_AS(parse_webhook(R"({"type":"email.sent"})"), std::invalid_argument);
  CHECK_THROWS_AS(parse_webhook(R"({"type":"email.sent","data":[]})"), std::invalid_argument);
  CHECK_THROWS_AS(parse_webhook(R"({"type":"","data":{}})"), std::invalid_argument);
}

TEST_CASE("resend: send uses the exact wire format", "[resend]") {
  Fixture f;
  f.http.push(200, R"({"id":"49a3999c-0ce1-4ea6-ab68-afcd6dc2e794"})");
  SendRequest r;
  r.from = "\"张三\" <zhang@team.example>";
  r.to = {"bob@team.example"};
  r.cc = {"carol@team.example"};
  r.bcc = {"dave@team.example"};
  r.reply_to = {"support@team.example"};
  r.subject = "周报";
  r.html = "<p>hi <img src=\"cid:logo@azmail\"></p>";
  r.text = "hi";
  r.headers = {{"X-AzMail-Ref", "uuid-1"}, {"In-Reply-To", "<p@x>"}};
  r.tags = {{"azmail_outbound", "uuid-1"}};
  r.attachments = {{"logo.png", "image/png", "iVBORw0KGgo=", std::string("logo@azmail")},
                   {"报告.pdf", "application/pdf", "JVBERi0=", std::nullopt}};
  r.scheduled_at_iso = "2026-10-08T09:00:00.000Z";
  r.idempotency_key = "uuid-1";
  CHECK(f.client.send(r) == "49a3999c-0ce1-4ea6-ab68-afcd6dc2e794");

  REQUIRE(f.http.seen.size() == 1);
  const auto& req = f.http.seen[0];
  CHECK(req.method == bhttp::verb::post);
  CHECK(req.url == "https://api.resend.test/emails");
  // RESEND_TIMEOUT_SEC plus the upload time of this body at RESEND_UPLOAD_KBPS (RT-4).
  CHECK(req.timeout == send_timeout(std::chrono::seconds(7), req.body.size(), 256u << 10));
  CHECK(req.timeout >= std::chrono::seconds(7));
  CHECK(req.timeout < std::chrono::seconds(8));
  CHECK(f.http.header(0, "authorization") == std::optional<std::string>("Bearer re_test_key"));
  CHECK(f.http.header(0, "user-agent") == std::optional<std::string>("azmail/0.1.0"));
  CHECK(f.http.header(0, "content-type") == std::optional<std::string>("application/json"));
  CHECK(f.http.header(0, "idempotency-key") == std::optional<std::string>("uuid-1"));
  CHECK_FALSE(f.http.header(0, "accept-encoding").has_value());

  const auto body = json::parse(req.body).as_object();
  CHECK(body.at("from").as_string() == "\"张三\" <zhang@team.example>");
  CHECK(body.at("to").as_array() == json::array{"bob@team.example"});
  CHECK(body.at("cc").as_array() == json::array{"carol@team.example"});
  CHECK(body.at("bcc").as_array() == json::array{"dave@team.example"});
  CHECK(body.at("reply_to").as_array() == json::array{"support@team.example"});
  CHECK(body.at("subject").as_string() == "周报");
  CHECK(body.at("html").as_string() == "<p>hi <img src=\"cid:logo@azmail\"></p>");
  CHECK(body.at("text").as_string() == "hi");
  CHECK(body.at("headers").as_object() == json::object{{"X-AzMail-Ref", "uuid-1"}, {"In-Reply-To", "<p@x>"}});
  CHECK(body.at("tags").as_array() == json::array{json::object{{"name", "azmail_outbound"}, {"value", "uuid-1"}}});
  const auto& atts = body.at("attachments").as_array();
  REQUIRE(atts.size() == 2);
  CHECK(atts[0].as_object() == json::object{{"filename", "logo.png"}, {"content", "iVBORw0KGgo="},
                                            {"content_type", "image/png"}, {"content_id", "logo@azmail"}});
  CHECK_FALSE(atts[1].as_object().contains("content_id"));
  CHECK(body.at("scheduled_at").as_string() == "2026-10-08T09:00:00.000Z");

  // Minimal request: optional members omitted, no Idempotency-Key when empty.
  f.http.push(200, R"({"id":"re_2"})");
  SendRequest m;
  m.from = "a@team.example";
  m.to = {"b@x.example"};
  m.subject = "";
  m.text = "t";
  CHECK(f.client.send(m) == "re_2");
  const auto b2 = json::parse(f.http.seen[1].body).as_object();
  for (std::string_view k : {"cc", "bcc", "reply_to", "headers", "tags", "attachments", "scheduled_at", "html"})
    CHECK_FALSE(b2.contains(k));
  CHECK(b2.at("subject").as_string() == "");
  CHECK_FALSE(f.http.header(1, "idempotency-key").has_value());

  // A 2xx without an id is a server error.
  f.http.push(200, R"({})");
  CHECK(capture([&] { (void)f.client.send(m); }).name == "invalid_response");
  f.http.push(200, "garbage");
  CHECK(capture([&] { (void)f.client.send(m); }).kind == Error::Kind::Server);
}

TEST_CASE("resend: get parses Postgres-style dates and nullable fields", "[resend]") {
  Fixture f;
  f.http.push(200, R"({"object":"email","id":"re/1","message_id":"<0100abc@email.amazonses.com>",
                      "last_event":"delivered","created_at":"2026-04-03 22:13:42.674981+00",
                      "scheduled_at":null})");
  const auto e = f.client.get("re/1");
  CHECK(f.http.seen[0].url == "https://api.resend.test/emails/re%2F1");
  CHECK(f.http.seen[0].method == bhttp::verb::get);
  CHECK(f.http.seen[0].body.empty());
  CHECK_FALSE(f.http.header(0, "content-type").has_value());
  CHECK(e.id == "re/1");
  CHECK(e.message_id == std::optional<std::string>("<0100abc@email.amazonses.com>"));
  CHECK(e.last_event == std::optional<std::string>("delivered"));
  CHECK(e.created_at_ms == *utc_ms(2026, 4, 3, 22, 13, 42, 674));
  CHECK_FALSE(e.scheduled_at_ms.has_value());

  f.http.push(200, R"({"id":"re_2","message_id":null,"last_event":"scheduled",
                      "created_at":"garbage","scheduled_at":"2026-10-08T09:00:00Z"})");
  const auto s = f.client.get("re_2", Priority::High);
  CHECK_FALSE(s.message_id.has_value());
  CHECK(s.created_at_ms == 0);
  CHECK(s.scheduled_at_ms == *utc_ms(2026, 10, 8, 9, 0, 0));
}

TEST_CASE("resend: update_schedule, cancel", "[resend]") {
  Fixture f;
  f.client.update_schedule("re_1", "2026-10-09T10:00:00.000Z");
  CHECK(f.http.seen[0].method == bhttp::verb::patch);
  CHECK(f.http.seen[0].url == "https://api.resend.test/emails/re_1");
  CHECK(json::parse(f.http.seen[0].body).as_object() == json::object{{"scheduled_at", "2026-10-09T10:00:00.000Z"}});
  f.client.cancel("re_1");
  CHECK(f.http.seen[1].method == bhttp::verb::post);
  CHECK(f.http.seen[1].url == "https://api.resend.test/emails/re_1/cancel");
  CHECK(f.http.seen[1].body.empty());

  f.http.push(422, R"({"name":"validation_error","message":"Email is not scheduled"})");
  const auto e = capture([&] { f.client.cancel("re_1"); });
  CHECK(e.kind == Error::Kind::Validation);
  CHECK(e.message == "Email is not scheduled");
}

TEST_CASE("resend: get_received parses the receiving API leniently", "[resend]") {
  Fixture f;
  f.http.push(200, R"({
    "object":"email","id":"in_1","from":"\"Ext\" <ext@outside.example>","subject":"Hello",
    "message_id":"<ext-1@outside.example>","to":["support@team.example"],"cc":"cc@team.example",
    "bcc":["hidden@team.example"],"reply_to":[],"received_for":["support@team.example","bob@team.example"],
    "html":"<p><img src=\"cid:ii_1\"></p>","text":null,
    "headers":{"In-Reply-To":"<p@x>","References":["<a@x>","<b@x>"],"X-Count":3},
    "authentication":{"spf":"PASS","dkim":{"result":"pass"},"dmarc":{"status":"Fail"}},
    "raw":{"download_url":"https://dl.example/raw?sig=1","expires_at":"2026-10-07T13:00:00Z"},
    "created_at":"2026-10-07T12:00:00.000Z",
    "attachments":[{"id":"att_1","filename":"a.png","content_type":"image/png",
                    "content_disposition":"INLINE","content_id":"<ii_1>","size":1234}]})");
  const auto e = f.client.get_received("in_1");
  CHECK(f.http.seen[0].url == "https://api.resend.test/emails/receiving/in_1?html_format=cid");
  CHECK(e.id == "in_1");
  CHECK(e.from == "\"Ext\" <ext@outside.example>");
  CHECK(e.subject == "Hello");
  CHECK(e.message_id == "<ext-1@outside.example>");
  CHECK(e.to == std::vector<std::string>{"support@team.example"});
  CHECK(e.cc == std::vector<std::string>{"cc@team.example"});
  CHECK(e.bcc == std::vector<std::string>{"hidden@team.example"});
  CHECK(e.reply_to.empty());
  CHECK(e.received_for == std::vector<std::string>{"support@team.example", "bob@team.example"});
  CHECK(e.html == std::optional<std::string>("<p><img src=\"cid:ii_1\"></p>"));
  CHECK_FALSE(e.text.has_value());
  CHECK(e.headers == std::vector<std::pair<std::string, std::string>>{
                         {"in-reply-to", "<p@x>"}, {"references", "<a@x>"}, {"references", "<b@x>"}, {"x-count", "3"}});
  CHECK(e.auth.spf == std::optional<std::string>("pass"));
  CHECK(e.auth.dkim == std::optional<std::string>("pass"));
  CHECK(e.auth.dmarc == std::optional<std::string>("fail"));
  CHECK(e.raw_download_url == std::optional<std::string>("https://dl.example/raw?sig=1"));
  CHECK(e.created_at_ms == *utc_ms(2026, 10, 7, 12, 0, 0));
  REQUIRE(e.attachments.size() == 1);
  CHECK(e.attachments[0].id == "att_1");
  CHECK(e.attachments[0].content_disposition == "inline");
  CHECK(e.attachments[0].content_id == std::optional<std::string>("ii_1"));
  CHECK(e.attachments[0].size == 1234);

  // Header arrays [{name,value}], object addresses, raw_download_url fallback, no auth.
  f.http.push(200, R"({"id":"in_2","from":{"email":"x@y.example","name":"X"},
                      "headers":[{"name":"Message-ID","value":"<h@x>"}],"raw_download_url":"https://dl/2"})");
  const auto e2 = f.client.get_received("in_2");
  CHECK(e2.from == "\"X\" <x@y.example>");
  CHECK(e2.headers == std::vector<std::pair<std::string, std::string>>{{"message-id", "<h@x>"}});
  CHECK(e2.raw_download_url == std::optional<std::string>("https://dl/2"));
  CHECK_FALSE(e2.auth.spf.has_value());
  CHECK(e2.attachments.empty());

  f.http.push(404, R"({"name":"not_found","message":"Email not found"})");
  CHECK(capture([&] { (void)f.client.get_received("in_3"); }).kind == Error::Kind::NotFound);
}

TEST_CASE("resend: list_received and list_received_attachments", "[resend]") {
  Fixture f;
  f.http.push(200, R"({"object":"list","has_more":true,"data":[{"id":"in_3"},{"id":"in_2"},{"nope":1}]})");
  auto page = f.client.list_received(500, std::string("in 9"), std::nullopt);
  CHECK(f.http.seen[0].url == "https://api.resend.test/emails/receiving?limit=100&after=in%209");
  CHECK(page.ids == std::vector<std::string>{"in_3", "in_2"});
  CHECK(page.has_more);
  f.http.push(200, R"({"data":[]})");
  page = f.client.list_received(0, std::nullopt, std::string("in_1"));
  CHECK(f.http.seen[1].url == "https://api.resend.test/emails/receiving?limit=1&before=in_1");
  CHECK(page.ids.empty());
  CHECK_FALSE(page.has_more);
  f.http.push(200, R"({"object":"list"})");
  CHECK(capture([&] { (void)f.client.list_received(10, {}, {}); }).name == "invalid_response");

  f.http.push(200, R"({"object":"list","data":[{"id":"att_1","filename":"f.txt","content_type":"text/plain",
                       "content_disposition":"attachment","size":"12","download_url":"https://dl/att_1"}]})");
  const auto atts = f.client.list_received_attachments("in_1");
  CHECK(f.http.seen.back().url == "https://api.resend.test/emails/receiving/in_1/attachments?limit=100");
  REQUIRE(atts.size() == 1);
  CHECK(atts[0].filename == "f.txt");
  CHECK(atts[0].size == 12);
  CHECK(atts[0].download_url == "https://dl/att_1");
  CHECK_FALSE(atts[0].content_id.has_value());
  f.http.push(200, R"([{"id":"att_2","filename":"g"}])");  // bare array tolerated
  CHECK(f.client.list_received_attachments("in_1").at(0).content_type == "application/octet-stream");
}

TEST_CASE("resend: download_to never sends credentials and maps failures", "[resend]") {
  Fixture f;
  net::HttpResponse ok = ScriptedHttp::reply(200, "");
  ok.body_size = 42;
  ok.sink_sha256 = std::string(64, 'a');
  f.http.replies.push_back(ok);
  const auto r = f.client.download_to("https://dl.example/x?sig=1", "/tmp/azmail-never-written", 1000);
  CHECK(r.size == 42);
  CHECK(r.sha256 == std::string(64, 'a'));
  const auto& req = f.http.seen[0];
  CHECK(req.url == "https://dl.example/x?sig=1");
  CHECK(req.sink == std::optional<std::filesystem::path>("/tmp/azmail-never-written"));
  CHECK(req.max_body == 1000);
  CHECK(req.max_redirects > 0);
  CHECK_FALSE(f.http.header(0, "authorization").has_value());
  CHECK(f.http.header(0, "user-agent") == std::optional<std::string>("azmail/0.1.0"));
  CHECK(f.limiter.available() == 100.0);  // downloads are not rate-limited

  f.http.replies.push_back(ok);
  CHECK(f.client.download("https://dl.example/y", "/tmp/azmail-never-written", 1000) == 42);

  f.http.throw_next = net::NetError(net::NetError::Kind::TooLarge, "big");
  auto e = capture([&] { (void)f.client.download_to("https://dl/x", "/tmp/x", 10); });
  CHECK(e.kind == Error::Kind::Validation);
  CHECK(e.name == "too_large");
  f.http.push(403, "<Error><Code>AccessDenied</Code></Error>");
  e = capture([&] { (void)f.client.download_to("https://dl/x", "/tmp/x", 10); });
  CHECK(e.kind == Error::Kind::Server);  // retried by the job with fresh URLs
  CHECK(e.name == "download_failed");
  CHECK(e.http_status == 403);
  f.http.push(429, R"({"name":"rate_limit_exceeded"})", {{"retry-after", "2"}});
  e = capture([&] { (void)f.client.download_to("https://dl/x", "/tmp/x", 10); });
  CHECK(e.kind == Error::Kind::RateLimited);
  CHECK(e.retry_after == std::chrono::seconds(2));
}

TEST_CASE("resend: 429 pauses the shared limiter; network errors map to Network", "[resend]") {
  Fixture f;
  f.http.push(429, R"({"statusCode":429,"name":"rate_limit_exceeded","message":"slow down"})",
              {{"retry-after", "4"}});
  const auto e = capture([&] { (void)f.client.get("re_1"); });
  CHECK(e.kind == Error::Kind::RateLimited);
  CHECK(e.retry_after == std::chrono::seconds(4));
  CHECK(f.limiter.paused_until() == f.clock.now_ms() + 4000);

  f.clock.advance(5000);
  f.http.throw_next = net::NetError(net::NetError::Kind::Timeout, "timed out waiting for response");
  const auto n = capture([&] { (void)f.client.get("re_1"); });
  CHECK(n.kind == Error::Kind::Network);
  CHECK(n.http_status == 0);
  CHECK(n.retryable());
  CHECK_THAT(n.message, ContainsSubstring("timed out"));
}

TEST_CASE("resend: calls stop promptly when the job's stop token fires", "[resend]") {
  Fixture f;
  f.limiter.pause_for(std::chrono::hours(1));
  std::stop_source src;
  src.request_stop();
  ScopedStopToken scope(src.get_token());
  const auto e = capture([&] { (void)f.client.get("re_1"); });
  CHECK(e.kind == Error::Kind::Network);
  CHECK(e.name == "stopped");
  CHECK(f.http.seen.empty());
}

TEST_CASE("resend: domains", "[resend]") {
  Fixture f;
  f.http.push(200, R"({"data":[{"id":"d1","name":"team.example","status":"verified","region":"us-east-1",
                       "created_at":"2026-01-01T00:00:00Z"}]})");
  const auto list = f.client.list_domains();
  REQUIRE(list.size() == 1);
  CHECK(list[0].id == "d1");
  CHECK(list[0].region == std::optional<std::string>("us-east-1"));
  CHECK(list[0].created_at_ms == *utc_ms(2026, 1, 1));
  CHECK(list[0].records.empty());
  f.http.push(200, R"({"id":"d1","name":"team.example","status":"pending","records":[
      {"record":"MX","name":"send","type":"MX","ttl":"Auto","status":"verified","value":"feedback-smtp","priority":10},
      {"record":"DKIM","name":"resend._domainkey","type":"TXT","ttl":3600,"status":"pending","value":"p=MIG"}]})");
  const auto d = f.client.get_domain("d1");
  CHECK(f.http.seen.back().url == "https://api.resend.test/domains/d1");
  REQUIRE(d.records.size() == 2);
  CHECK(d.records[0].priority == std::optional<int>(10));
  CHECK(d.records[0].ttl == "Auto");
  CHECK(d.records[1].ttl == "3600");
  CHECK_FALSE(d.records[1].priority.has_value());
  CHECK_FALSE(d.region.has_value());
}

TEST_CASE("resend: a test double without transport reports NotImplemented", "[resend]") {
  struct Double final : Client {};
  Double d;
  CHECK_THROWS_AS(d.cancel("x"), NotImplemented);
  CHECK_THROWS_AS(d.get("x"), NotImplemented);
}

TEST_CASE("resend: end-to-end over HTTP sends UA, auth and Idempotency-Key on the wire",
          "[resend][http_client]") {
  test::FakeHttpServer srv([](const test::FakeRequest& req) {
    if (req.path == "/emails" && req.method == "POST") return test::FakeResponse::json(200, R"({"id":"re_wire"})");
    return test::FakeResponse::json(404, R"({"name":"not_found","message":"nope"})");
  });
  Config cfg = test_config();
  cfg.resend_api_base = srv.base_url();
  cfg.allow_insecure_http = true;
  net::HttpClient http(net::client_options_from(cfg));
  RateLimiter limiter({.rps = 50, .burst = 50});
  Client client(cfg, http, limiter);
  SendRequest r;
  r.from = "a@team.example";
  r.to = {"b@x.example"};
  r.subject = "s";
  r.text = "t";
  r.idempotency_key = "key-123";
  CHECK(client.send(r) == "re_wire");
  const auto seen = srv.requests().at(0);
  CHECK(seen.header("user-agent") == std::optional<std::string>("azmail/0.1.0"));
  CHECK(seen.header("authorization") == std::optional<std::string>("Bearer re_test_key"));
  CHECK(seen.header("idempotency-key") == std::optional<std::string>("key-123"));
  CHECK(seen.header("content-type") == std::optional<std::string>("application/json"));
  CHECK_FALSE(seen.has_header("accept-encoding"));
  CHECK(json::parse(seen.body).as_object().at("to").as_array() == json::array{"b@x.example"});
  CHECK(capture([&] { (void)client.get("missing"); }).kind == Error::Kind::NotFound);
}

// ---- RT-2: attachment listing is paginated -------------------------------------------------------

TEST_CASE("resend: list_received_attachments follows has_more with after=<last id> (RT-2)", "[resend]") {
  Fixture f;
  f.http.push(200, R"({"object":"list","has_more":true,"data":[{"id":"att_1","filename":"a"},{"id":"att_2","filename":"b"}]})");
  f.http.push(200, R"({"object":"list","has_more":false,"data":[{"id":"att_3","filename":"c"}]})");
  const auto atts = f.client.list_received_attachments("in 1");
  REQUIRE(f.http.seen.size() == 2);
  CHECK(f.http.seen[0].url == "https://api.resend.test/emails/receiving/in%201/attachments?limit=100");
  CHECK(f.http.seen[1].url == "https://api.resend.test/emails/receiving/in%201/attachments?limit=100&after=att_2");
  REQUIRE(atts.size() == 3);
  CHECK(atts[0].id == "att_1");
  CHECK(atts[2].id == "att_3");

  // A server that keeps answering has_more with the same page: the walk ends, nothing duplicated.
  Fixture g;
  for (int i = 0; i < 5; ++i) g.http.push(200, R"({"has_more":true,"data":[{"id":"att_1","filename":"a"}]})");
  CHECK(g.client.list_received_attachments("in_1").size() == 1);
  CHECK(g.http.seen.size() == 2);

  // Bounded: at most 20 pages however long has_more stays true.
  Fixture h;
  for (int i = 0; i < 30; ++i)
    h.http.push(200, R"({"has_more":true,"data":[{"id":"att_)" + std::to_string(i) + R"(","filename":"x"}]})");
  CHECK(h.client.list_received_attachments("in_1").size() == 20);
  CHECK(h.http.seen.size() == 20);
}

// ---- RT-4: POST /emails deadline grows with the body -------------------------------------------

TEST_CASE("resend: send_timeout covers uploading the body (RT-4)", "[resend]") {
  using std::chrono::milliseconds;
  using std::chrono::seconds;
  CHECK(send_timeout(seconds(30), 0, 256u << 10) == seconds(30));
  CHECK(send_timeout(seconds(30), 256u << 10, 256u << 10) == seconds(31));
  // ~37 MB of base64 JSON (28 MiB of attachments) at 256 KiB/s: 30 s + ~143 s.
  const auto big = send_timeout(seconds(30), 37'400'000, 256u << 10);
  CHECK(big > seconds(170));
  CHECK(big < seconds(180));
  // Capped at 15 min (or the base when that is larger); a zero rate cannot divide by zero.
  CHECK(send_timeout(seconds(30), 1u << 30, 1024) == std::chrono::minutes(15));
  CHECK(send_timeout(std::chrono::minutes(20), 1u << 30, 1024) == std::chrono::minutes(20));
  CHECK(send_timeout(seconds(1), 10, 0) >= seconds(1));

  Fixture f;
  f.cfg.resend_upload_kbps = 64;
  Client client(f.cfg, f.http, f.limiter);
  f.http.push(200, R"({"id":"re_big"})");
  SendRequest r;
  r.from = "a@team.example";
  r.to = {"b@x.example"};
  r.subject = "s";
  r.html = std::string(2u << 20, 'a');  // 2 MiB body: 32 s at 64 KiB/s
  CHECK(client.send(r) == "re_big");
  const auto& req = f.http.seen.back();
  CHECK(req.timeout >= seconds(7 + 32));
  CHECK(req.timeout < seconds(7 + 33));
  CHECK(req.response_timeout == std::optional<std::chrono::milliseconds>(req.timeout));  // send buffer drains
  // Other calls keep RESEND_TIMEOUT_SEC.
  f.http.push(200, R"({"id":"re_big","object":"email","created_at":"2026-01-01T00:00:00Z"})");
  (void)client.get("re_big");
  CHECK(f.http.seen.back().timeout == seconds(7));
  CHECK_FALSE(f.http.seen.back().response_timeout.has_value());
}

namespace {

// A plain-HTTP server that reads the request body slowly (`chunk` bytes every `pause`), then
// answers 200 with `reply`. One connection.
struct SlowReader {
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::acceptor acc{ioc, {boost::asio::ip::make_address("127.0.0.1"), 0}};
  std::thread th;
  std::atomic<std::size_t> received{0};

  SlowReader(std::size_t chunk, std::chrono::milliseconds pause, std::string reply) {
    th = std::thread([this, chunk, pause, reply = std::move(reply)] {
      try {
        auto sock = acc.accept();
        std::string head;
        char c;
        while (head.find("\r\n\r\n") == std::string::npos) {
          boost::asio::read(sock, boost::asio::buffer(&c, 1));
          head.push_back(c);
        }
        std::size_t len = 0;
        const auto lower = to_lower_ascii(head);
        if (auto p = lower.find("content-length:"); p != std::string::npos) len = std::stoul(lower.substr(p + 15));
        std::vector<char> buf(chunk);
        while (received < len) {
          const std::size_t n = sock.read_some(boost::asio::buffer(buf.data(), std::min(chunk, len - received)));
          received += n;
          std::this_thread::sleep_for(pause);
        }
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply;
        boost::asio::write(sock, boost::asio::buffer(resp));
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      } catch (const std::exception&) {
        // the client gave up
      }
    });
  }
  ~SlowReader() {
    boost::system::error_code ec;
    acc.close(ec);
    if (th.joinable()) th.join();
  }
  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(acc.local_endpoint().port()); }
};

}  // namespace

TEST_CASE("resend: a large POST /emails slower than RESEND_TIMEOUT_SEC still succeeds (RT-4)",
          "[resend][http_client][timeout]") {
  // ~1.5 MB uploaded at ~640 KB/s ≈ 2.3 s, with RESEND_TIMEOUT_SEC=1 (idle limit per chunk).
  SlowReader srv(64u << 10, std::chrono::milliseconds(100), R"({"id":"re_slow"})");
  Config cfg = test_config();
  cfg.resend_api_base = srv.base_url();
  cfg.allow_insecure_http = true;
  cfg.resend_timeout_sec = 1;
  cfg.resend_upload_kbps = 256;
  net::HttpClient http(net::client_options_from(cfg));
  RateLimiter limiter({.rps = 50, .burst = 50});
  Client client(cfg, http, limiter);
  SendRequest r;
  r.from = "a@team.example";
  r.to = {"b@x.example"};
  r.subject = "s";
  r.html = std::string(1'500'000, 'a');
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(client.send(r) == "re_slow");
  CHECK(std::chrono::steady_clock::now() - t0 > std::chrono::seconds(1));  // really slower than the base
  CHECK(srv.received >= 1'500'000u);
}

TEST_CASE("resend: a shutdown abort reads as 'stopped' (RT-7)", "[resend]") {
  Fixture f;
  f.http.throw_next = net::NetError(net::NetError::Kind::Aborted, "aborted");
  const auto e = capture([&] { (void)f.client.get("re_1"); });
  CHECK(e.kind == Error::Kind::Network);
  CHECK(e.name == "stopped");
}
