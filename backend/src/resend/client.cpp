// Owner: WP-C
// Typed Resend API client (DESIGN §3, B1–B10): exact wire format, rate limiting, error mapping.
#include "resend/client.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "core/time.hpp"
#include "net/http_client.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>

namespace azm::resend {
namespace {

namespace bhttp = boost::beast::http;
namespace json = boost::json;

constexpr std::size_t kMaxJsonResponse = 64u << 20;  // received mail bodies can be large
constexpr std::chrono::minutes kDownloadTimeout{15};
constexpr int kDownloadRedirects = 5;

std::string enc(std::string_view id) { return url_encode(id); }

std::optional<std::string> str_field(const json::object& o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().as_string());
}

std::string str_or(const json::object& o, std::string_view key, std::string_view dflt = {}) {
  return str_field(o, key).value_or(std::string(dflt));
}

// Address fields come as a string, an array of strings, or {email,name} objects.
std::vector<std::string> addr_list(const json::object& o, std::string_view key) {
  std::vector<std::string> out;
  auto it = o.find(key);
  if (it == o.end()) return out;
  auto one = [&](const json::value& v) {
    if (v.is_string()) {
      if (!v.as_string().empty()) out.emplace_back(v.as_string());
    } else if (v.is_object()) {
      const auto& a = v.as_object();
      const std::string email = str_or(a, "email", str_or(a, "address"));
      const std::string name = str_or(a, "name");
      if (!email.empty()) out.push_back(name.empty() ? email : "\"" + name + "\" <" + email + ">");
    }
  };
  if (it->value().is_array()) {
    for (const auto& v : it->value().as_array()) one(v);
  } else {
    one(it->value());
  }
  return out;
}

std::optional<int64_t> int_field(const json::object& o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end()) return std::nullopt;
  const auto& v = it->value();
  if (v.is_int64()) return v.as_int64();
  if (v.is_uint64()) return static_cast<int64_t>(v.as_uint64());
  if (v.is_double()) return static_cast<int64_t>(v.as_double());
  if (v.is_string()) {
    try {
      return std::stoll(std::string(v.as_string()));
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::string scalar_text(const json::value& v) {
  if (v.is_string()) return std::string(v.as_string());
  if (v.is_int64()) return std::to_string(v.as_int64());
  if (v.is_uint64()) return std::to_string(v.as_uint64());
  if (v.is_double()) return json::serialize(v);
  if (v.is_bool()) return v.as_bool() ? "true" : "false";
  return {};
}

int64_t time_field(const json::object& o, std::string_view key) {
  if (auto s = str_field(o, key)) return parse_iso8601_lenient(*s).value_or(0);
  return 0;
}

// "<id@host>" → "id@host" (content ids are stored without brackets).
std::string strip_angle(std::string_view s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '<' && s.back() == '>') s = s.substr(1, s.size() - 2);
  return std::string(trim(s));
}

// Authentication verdicts: "pass" or {result|status|verdict:"pass"}, lowercased.
std::optional<std::string> verdict(const json::object& o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end()) return std::nullopt;
  std::string v;
  if (it->value().is_string()) {
    v = std::string(it->value().as_string());
  } else if (it->value().is_object()) {
    const auto& a = it->value().as_object();
    v = str_field(a, "result").value_or(str_field(a, "status").value_or(str_or(a, "verdict")));
  }
  v = to_lower_ascii(trim(v));
  if (v.empty()) return std::nullopt;
  return v;
}

RecvAttachment parse_attachment(const json::object& a) {
  RecvAttachment r;
  r.id = str_or(a, "id");
  r.filename = str_or(a, "filename", str_or(a, "name"));
  r.content_type = str_or(a, "content_type", "application/octet-stream");
  r.content_disposition = to_lower_ascii(str_or(a, "content_disposition", "attachment"));
  if (auto cid = str_field(a, "content_id")) {
    std::string c = strip_angle(*cid);
    if (!c.empty()) r.content_id = std::move(c);
  }
  r.size = int_field(a, "size").value_or(0);
  r.download_url = str_or(a, "download_url");
  return r;
}

// `data` array of a list response, or the response itself when it is a bare array.
const json::array* list_data(const json::value& v) {
  if (v.is_array()) return &v.as_array();
  if (v.is_object()) {
    auto it = v.as_object().find("data");
    if (it != v.as_object().end() && it->value().is_array()) return &it->value().as_array();
  }
  return nullptr;
}

DomainInfo parse_domain(const json::object& d) {
  DomainInfo info;
  info.id = str_or(d, "id");
  info.name = str_or(d, "name");
  info.status = str_or(d, "status");
  if (auto r = str_field(d, "region")) info.region = std::move(r);
  if (auto ca = str_field(d, "created_at")) info.created_at_ms = parse_iso8601_lenient(*ca);
  if (auto it = d.find("records"); it != d.end() && it->value().is_array()) {
    for (const auto& rv : it->value().as_array()) {
      if (!rv.is_object()) continue;
      const auto& r = rv.as_object();
      DomainRecord rec;
      rec.record = str_or(r, "record");
      rec.name = str_or(r, "name");
      rec.type = str_or(r, "type");
      if (auto t = r.find("ttl"); t != r.end()) rec.ttl = scalar_text(t->value());
      rec.status = str_or(r, "status");
      rec.value = str_or(r, "value");
      if (auto p = int_field(r, "priority")) rec.priority = static_cast<int>(*p);
      info.records.push_back(std::move(rec));
    }
  }
  return info;
}

}  // namespace

struct Client::Impl {
  std::string base;  // no trailing '/'
  std::string api_key;
  std::string user_agent;
  std::chrono::milliseconds timeout{30000};
  net::HttpClient* http = nullptr;
  RateLimiter* limiter = nullptr;

  void require(std::string_view method) const {
    if (http == nullptr)
      throw NotImplemented("resend::Client::" + std::string(method) + " (test double without a transport)");
  }

  [[noreturn]] static void network_error(const net::NetError& e) {
    throw Error(Error::Kind::Network, 0, "network", e.what());
  }

  // One API call: rate-limiter token, auth headers, error mapping. Returns a 2xx response.
  net::HttpResponse call(bhttp::verb method, const std::string& path, const json::object* body,
                         Priority prio, std::vector<std::pair<std::string, std::string>> extra = {}) {
    if (!limiter->acquire(prio, current_stop_token()))
      throw Error(Error::Kind::Network, 0, "stopped", "shutdown requested");
    net::HttpRequest req;
    req.method = method;
    req.url = base + path;
    req.timeout = timeout;
    req.max_body = kMaxJsonResponse;
    req.headers.emplace_back("Authorization", "Bearer " + api_key);
    req.headers.emplace_back("User-Agent", user_agent);
    req.headers.emplace_back("Accept", "application/json");
    if (body != nullptr) {
      req.headers.emplace_back("Content-Type", "application/json");
      req.body = json::serialize(*body);
    }
    for (auto& h : extra) req.headers.push_back(std::move(h));
    net::HttpResponse resp;
    try {
      resp = http->send(req);
    } catch (const net::NetError& e) {
      log::warn("resend request failed", {{"method", std::string(bhttp::to_string(method))},
                                          {"path", path}, {"error", e.what()}});
      network_error(e);
    }
    if (resp.status >= 200 && resp.status < 300) return resp;
    const auto ra = resp.header("retry-after");
    Error err = classify_error(static_cast<int>(resp.status), resp.body,
                               ra ? std::optional<std::string_view>(*ra) : std::nullopt);
    if (err.kind == Error::Kind::RateLimited)
      limiter->pause_for(std::chrono::duration_cast<std::chrono::milliseconds>(
          err.retry_after.value_or(std::chrono::seconds(1))));
    log::warn("resend API error", {{"method", std::string(bhttp::to_string(method))},
                                   {"path", path},
                                   {"status", static_cast<int64_t>(resp.status)},
                                   {"kind", std::string(to_string(err.kind))},
                                   {"name", err.name}});
    throw err;
  }

  static json::value parse_body(const net::HttpResponse& resp) {
    boost::system::error_code ec;
    json::value v = json::parse(resp.body, ec);
    if (ec)
      throw Error(Error::Kind::Server, static_cast<int>(resp.status), "invalid_response",
                  "response is not valid JSON");
    return v;
  }
  static json::object parse_object(const net::HttpResponse& resp) {
    json::value v = parse_body(resp);
    if (!v.is_object())
      throw Error(Error::Kind::Server, static_cast<int>(resp.status), "invalid_response",
                  "response is not a JSON object");
    return std::move(v.as_object());
  }
};

Client::Client(const Config& cfg, net::HttpClient& http, RateLimiter& limiter)
    : impl_(std::make_unique<Impl>()) {
  std::string base = cfg.resend_api_base;
  while (!base.empty() && base.back() == '/') base.pop_back();
  impl_->base = std::move(base);
  impl_->api_key = cfg.resend_api_key;
  // Cloudflare in front of Resend rejects requests without a User-Agent (error 1010).
#ifdef AZMAIL_VERSION
  impl_->user_agent = cfg.resend_user_agent.empty() ? std::string("azmail/") + AZMAIL_VERSION : cfg.resend_user_agent;
#else
  impl_->user_agent = cfg.resend_user_agent.empty() ? std::string("azmail") : cfg.resend_user_agent;
#endif
  if (cfg.resend_timeout_sec > 0) impl_->timeout = std::chrono::seconds(cfg.resend_timeout_sec);
  impl_->http = &http;
  impl_->limiter = &limiter;
}
Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

std::string Client::send(const SendRequest& r) {
  impl_->require("send");
  json::object o;
  o["from"] = r.from;
  json::array to;
  for (const auto& a : r.to) to.emplace_back(a);
  o["to"] = std::move(to);
  o["subject"] = r.subject;
  if (!r.html.empty()) o["html"] = r.html;
  if (!r.text.empty()) o["text"] = r.text;
  auto list = [&](std::string_view key, const std::vector<std::string>& v) {
    if (v.empty()) return;
    json::array a;
    for (const auto& x : v) a.emplace_back(x);
    o[key] = std::move(a);
  };
  list("cc", r.cc);
  list("bcc", r.bcc);
  list("reply_to", r.reply_to);
  if (!r.headers.empty()) {
    json::object h;
    for (const auto& [k, v] : r.headers) h[k] = v;
    o["headers"] = std::move(h);
  }
  if (!r.tags.empty()) {
    json::array tags;
    for (const auto& [k, v] : r.tags) tags.push_back(json::object{{"name", k}, {"value", v}});
    o["tags"] = std::move(tags);
  }
  if (!r.attachments.empty()) {
    json::array atts;
    for (const auto& a : r.attachments) {
      json::object j;
      j["filename"] = a.filename;
      j["content"] = a.content_b64;
      if (!a.content_type.empty()) j["content_type"] = a.content_type;
      if (a.content_id && !a.content_id->empty()) j["content_id"] = *a.content_id;
      atts.push_back(std::move(j));
    }
    o["attachments"] = std::move(atts);
  }
  if (r.scheduled_at_iso) o["scheduled_at"] = *r.scheduled_at_iso;
  std::vector<std::pair<std::string, std::string>> extra;
  if (!r.idempotency_key.empty()) extra.emplace_back("Idempotency-Key", r.idempotency_key);
  const auto resp = impl_->call(bhttp::verb::post, "/emails", &o, Priority::High, std::move(extra));
  const json::object body = Impl::parse_object(resp);
  auto id = str_field(body, "id");
  if (!id || id->empty())
    throw Error(Error::Kind::Server, static_cast<int>(resp.status), "invalid_response", "send response has no id");
  return *id;
}

SentEmail Client::get(std::string_view id, Priority p) {
  impl_->require("get");
  const auto resp = impl_->call(bhttp::verb::get, "/emails/" + enc(id), nullptr, p);
  const json::object o = Impl::parse_object(resp);
  SentEmail e;
  e.id = str_or(o, "id", id);
  if (auto mid = str_field(o, "message_id"); mid && !trim(*mid).empty()) e.message_id = std::move(mid);
  if (auto le = str_field(o, "last_event"); le && !le->empty()) e.last_event = std::move(le);
  if (auto sa = str_field(o, "scheduled_at"); sa && !sa->empty()) e.scheduled_at_ms = parse_iso8601_lenient(*sa);
  e.created_at_ms = time_field(o, "created_at");
  return e;
}

void Client::update_schedule(std::string_view id, std::string_view iso) {
  impl_->require("update_schedule");
  const json::object body{{"scheduled_at", iso}};
  (void)impl_->call(bhttp::verb::patch, "/emails/" + enc(id), &body, Priority::High);
}

void Client::cancel(std::string_view id) {
  impl_->require("cancel");
  (void)impl_->call(bhttp::verb::post, "/emails/" + enc(id) + "/cancel", nullptr, Priority::High);
}

ReceivedEmail Client::get_received(std::string_view id) {
  impl_->require("get_received");
  const auto resp =
      impl_->call(bhttp::verb::get, "/emails/receiving/" + enc(id) + "?html_format=cid", nullptr, Priority::Normal);
  const json::object o = Impl::parse_object(resp);
  ReceivedEmail e;
  e.id = str_or(o, "id", id);
  {
    auto from = addr_list(o, "from");
    if (!from.empty()) e.from = from.front();
  }
  e.subject = str_or(o, "subject");
  e.message_id = str_or(o, "message_id");
  e.to = addr_list(o, "to");
  e.cc = addr_list(o, "cc");
  e.bcc = addr_list(o, "bcc");
  e.reply_to = addr_list(o, "reply_to");
  e.received_for = addr_list(o, "received_for");
  e.html = str_field(o, "html");
  e.text = str_field(o, "text");
  if (auto it = o.find("headers"); it != o.end()) {
    auto add = [&](std::string_view name, const json::value& v) {
      const std::string n = to_lower_ascii(trim(name));
      if (n.empty()) return;
      if (v.is_array()) {  // repeated headers
        for (const auto& x : v.as_array()) e.headers.emplace_back(n, scalar_text(x));
      } else {
        e.headers.emplace_back(n, scalar_text(v));
      }
    };
    if (it->value().is_object()) {
      for (const auto& [k, v] : it->value().as_object()) add(k, v);
    } else if (it->value().is_array()) {
      for (const auto& h : it->value().as_array()) {
        if (!h.is_object()) continue;
        const auto& ho = h.as_object();
        auto n = ho.find("name");
        auto v = ho.find("value");
        if (n != ho.end() && n->value().is_string() && v != ho.end()) add(n->value().as_string(), v->value());
      }
    }
  }
  for (std::string_view key : {"authentication", "auth", "authentication_results"}) {
    auto it = o.find(key);
    if (it == o.end() || !it->value().is_object()) continue;
    const auto& a = it->value().as_object();
    e.auth.spf = verdict(a, "spf");
    e.auth.dkim = verdict(a, "dkim");
    e.auth.dmarc = verdict(a, "dmarc");
    break;
  }
  if (auto it = o.find("raw"); it != o.end() && it->value().is_object()) {
    if (auto u = str_field(it->value().as_object(), "download_url"); u && !u->empty()) e.raw_download_url = std::move(u);
  }
  if (!e.raw_download_url) {
    if (auto u = str_field(o, "raw_download_url"); u && !u->empty()) e.raw_download_url = std::move(u);
  }
  e.created_at_ms = time_field(o, "created_at");
  if (auto it = o.find("attachments"); it != o.end() && it->value().is_array()) {
    for (const auto& a : it->value().as_array())
      if (a.is_object()) e.attachments.push_back(parse_attachment(a.as_object()));
  }
  return e;
}

ReceivedPage Client::list_received(int limit, std::optional<std::string> after,
                                   std::optional<std::string> before) {
  impl_->require("list_received");
  std::string path = "/emails/receiving?limit=" + std::to_string(std::clamp(limit, 1, 100));
  if (after && !after->empty()) path += "&after=" + enc(*after);
  if (before && !before->empty()) path += "&before=" + enc(*before);
  const auto resp = impl_->call(bhttp::verb::get, path, nullptr, Priority::Low);
  const json::value v = Impl::parse_body(resp);
  ReceivedPage page;
  if (const auto* data = list_data(v)) {
    for (const auto& item : *data) {
      if (item.is_string()) page.ids.emplace_back(item.as_string());
      else if (item.is_object())
        if (auto id = str_field(item.as_object(), "id"); id && !id->empty()) page.ids.push_back(*id);
    }
  } else {
    throw Error(Error::Kind::Server, static_cast<int>(resp.status), "invalid_response", "list response has no data");
  }
  if (v.is_object()) {
    auto it = v.as_object().find("has_more");
    page.has_more = it != v.as_object().end() && it->value().is_bool() && it->value().as_bool();
  }
  return page;
}

std::vector<RecvAttachment> Client::list_received_attachments(std::string_view id) {
  impl_->require("list_received_attachments");
  const auto resp =
      impl_->call(bhttp::verb::get, "/emails/receiving/" + enc(id) + "/attachments", nullptr, Priority::Normal);
  const json::value v = Impl::parse_body(resp);
  std::vector<RecvAttachment> out;
  const auto* data = list_data(v);
  if (data == nullptr)
    throw Error(Error::Kind::Server, static_cast<int>(resp.status), "invalid_response",
                "attachments response has no data");
  for (const auto& a : *data)
    if (a.is_object()) out.push_back(parse_attachment(a.as_object()));
  return out;
}

DownloadResult Client::download_to(std::string_view url, const std::filesystem::path& dest,
                                   std::size_t max_bytes) {
  impl_->require("download_to");
  net::HttpRequest req;
  req.url = std::string(url);
  req.sink = dest;
  req.max_body = max_bytes;
  req.max_redirects = kDownloadRedirects;
  req.timeout = kDownloadTimeout;
  req.headers.emplace_back("User-Agent", impl_->user_agent);  // never Authorization (B7, mock /_dl)
  net::HttpResponse resp;
  try {
    resp = impl_->http->send(req);
  } catch (const net::NetError& e) {
    if (e.kind == net::NetError::Kind::TooLarge)
      throw Error(Error::Kind::Validation, 0, "too_large", "download exceeds the size limit");
    Impl::network_error(e);
  }
  if (resp.status < 200 || resp.status >= 300) {
    if (resp.status == 429) {
      const auto ra = resp.header("retry-after");
      throw classify_error(429, resp.body, ra ? std::optional<std::string_view>(*ra) : std::nullopt);
    }
    // Expired / missing download URLs are retried by the job with freshly listed URLs.
    throw Error(Error::Kind::Server, static_cast<int>(resp.status), "download_failed",
                "download failed with HTTP " + std::to_string(resp.status));
  }
  DownloadResult r;
  r.size = static_cast<int64_t>(resp.body_size);
  r.sha256 = resp.sink_sha256.value_or("");
  return r;
}

int64_t Client::download(std::string_view url, const std::filesystem::path& dest, std::size_t max_bytes) {
  return download_to(url, dest, max_bytes).size;
}

std::vector<DomainInfo> Client::list_domains() {
  impl_->require("list_domains");
  const auto resp = impl_->call(bhttp::verb::get, "/domains", nullptr, Priority::Low);
  const json::value v = Impl::parse_body(resp);
  std::vector<DomainInfo> out;
  if (const auto* data = list_data(v))
    for (const auto& d : *data)
      if (d.is_object()) out.push_back(parse_domain(d.as_object()));
  return out;
}

DomainInfo Client::get_domain(std::string_view resend_domain_id) {
  impl_->require("get_domain");
  const auto resp = impl_->call(bhttp::verb::get, "/domains/" + enc(resend_domain_id), nullptr, Priority::Low);
  return parse_domain(Impl::parse_object(resp));
}

}  // namespace azm::resend
