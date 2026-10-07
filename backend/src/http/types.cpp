// Owner: WP-A
// Small, dependency-free implementations of the http/types.hpp helpers. Written in WP0 because
// every package's tests (WP-D handler tests in particular) construct Requests / Responses.
#include "http/types.hpp"

#include "core/json.hpp"
#include "core/strings.hpp"

#include <boost/json/serialize.hpp>
#include <boost/url/encoding_opts.hpp>
#include <boost/url/parse.hpp>

#include <charconv>
#include <stdexcept>

namespace azm::http {

std::optional<std::string_view> Request::header(std::string_view name) const {
  auto it = headers.find(boost::core::string_view(name.data(), name.size()));
  if (it == headers.end()) return std::nullopt;
  auto v = it->value();
  return std::string_view(v.data(), v.size());
}

Request Request::make(beast::http::verb method, std::string_view target) {
  auto parsed = boost::urls::parse_origin_form(boost::core::string_view(target.data(), target.size()));
  if (!parsed) throw std::invalid_argument("invalid request-target");
  Request r;
  r.method = method;
  r.target = std::string(target);
  r.url = boost::urls::url(*parsed);
  r.path = r.url.path();
  boost::urls::encoding_opts opt;
  opt.space_as_plus = false;
  for (auto p : r.url.params(opt)) {
    if (r.query.find(p.key) == r.query.end())
      r.query.emplace(p.key, p.has_value ? p.value : std::string());
  }
  return r;
}

Response Response::json(const boost::json::value& v, unsigned status) {
  Response r;
  r.status = status;
  r.body = boost::json::serialize(v);
  return r;
}

Response Response::no_content() {
  Response r;
  r.status = 204;
  r.content_type.clear();
  return r;
}

Response Response::error(unsigned status, std::string_view code, std::string_view msg,
                         boost::json::object details) {
  return from_error(ApiError(status, std::string(code), std::string(msg), std::move(details)));
}

Response Response::file(std::filesystem::path path, std::string content_type,
                        std::string disposition) {
  Response r;
  r.content_type = std::move(content_type);
  if (!disposition.empty()) r.headers.emplace_back("Content-Disposition", std::move(disposition));
  r.body = FileRef{std::move(path), false};
  return r;
}

Response Response::from_error(const ApiError& e) {
  Response r;
  r.status = e.status;
  r.body = boost::json::serialize(e.to_json());
  return r;
}

Response Response::text(std::string body, std::string content_type, unsigned status) {
  Response r;
  r.status = status;
  r.content_type = std::move(content_type);
  r.body = std::move(body);
  return r;
}

Response Response::redirect(std::string location, unsigned status) {
  Response r;
  r.status = status;
  r.content_type.clear();
  r.headers.emplace_back("Location", std::move(location));
  return r;
}

Response& Response::add_header(std::string name, std::string value) {
  headers.emplace_back(std::move(name), std::move(value));
  return *this;
}

std::optional<std::string_view> Response::find_header(std::string_view name) const {
  for (const auto& [k, v] : headers)
    if (iequals(k, name)) return std::string_view(v);
  return std::nullopt;
}

const Principal& Ctx::user() const {
  if (!principal) throw ApiError::unauthorized();
  return *principal;
}

namespace {
[[noreturn]] void bad_param(std::string_view field) {
  boost::json::object d;
  d["field"] = field;
  throw ApiError::bad_request("invalid_field", "参数无效", std::move(d));
}

std::optional<int64_t> parse_i64(std::string_view s) {
  int64_t v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
  return v;
}
}  // namespace

int64_t Ctx::id(std::string_view param) const {
  auto it = params.find(std::string(param));
  if (it == params.end()) bad_param(param);
  auto v = parse_i64(it->second);
  if (!v || *v <= 0) bad_param(param);
  return *v;
}

std::optional<std::string_view> Ctx::query(std::string_view key) const {
  auto it = req.query.find(std::string(key));
  if (it == req.query.end()) return std::nullopt;
  return std::string_view(it->second);
}

std::optional<int64_t> Ctx::query_int(std::string_view key) const {
  auto v = query(key);
  if (!v || v->empty()) return std::nullopt;
  auto n = parse_i64(*v);
  if (!n) bad_param(key);
  return n;
}

boost::json::object Ctx::body_object() const { return parse_json_object(req.body); }

}  // namespace azm::http
