#include "core/json.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <cmath>
#include <limits>

namespace azm {
namespace bj = boost::json;

namespace {

[[noreturn]] void throw_invalid_json() {
  throw ApiError::bad_request("invalid_json", "请求体不是有效的 JSON");
}

const bj::value* find(const bj::object& o, std::string_view key) {
  const auto it = o.find(key);
  if (it == o.end() || it->value().is_null()) return nullptr;
  return &it->value();
}

}  // namespace

bj::value parse_json(std::string_view text, std::size_t max_depth) {
  bj::parse_options opt;
  opt.max_depth = max_depth;
  boost::system::error_code ec;
  bj::value v = bj::parse(text, ec, bj::storage_ptr(), opt);
  if (ec) throw_invalid_json();
  return v;
}

bj::object parse_json_object(std::string_view text, std::size_t max_depth) {
  bj::value v = parse_json(text, max_depth);
  if (!v.is_object()) throw_invalid_json();
  return std::move(v.as_object());
}

std::string serialize_json(const bj::value& v) { return bj::serialize(v); }

void throw_invalid_field(std::string_view field) {
  bj::object details;
  details["field"] = field;
  throw ApiError::bad_request("invalid_field", "字段 " + std::string(field) + " 无效",
                              std::move(details));
}

std::optional<int64_t> as_int64(const bj::value& v) {
  if (v.is_int64()) return v.get_int64();
  if (v.is_uint64()) {
    const uint64_t u = v.get_uint64();
    if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    return static_cast<int64_t>(u);
  }
  if (v.is_double()) {
    const double d = v.get_double();
    // 2^63 is exactly representable; anything >= it is out of range.
    if (!std::isfinite(d) || d != std::floor(d) || d < -9223372036854775808.0 ||
        d >= 9223372036854775808.0)
      return std::nullopt;
    return static_cast<int64_t>(d);
  }
  return std::nullopt;
}

std::optional<std::string> opt_string(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return std::nullopt;
  if (!v->is_string()) throw_invalid_field(key);
  return std::string(v->get_string());
}

std::string req_string(const bj::object& o, std::string_view key) {
  auto s = opt_string(o, key);
  if (!s) throw_invalid_field(key);
  return std::move(*s);
}

std::optional<int64_t> opt_int64(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return std::nullopt;
  auto i = as_int64(*v);
  if (!i) throw_invalid_field(key);
  return i;
}

int64_t req_int64(const bj::object& o, std::string_view key) {
  auto i = opt_int64(o, key);
  if (!i) throw_invalid_field(key);
  return *i;
}

std::optional<double> opt_double(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return std::nullopt;
  if (v->is_double()) return v->get_double();
  if (v->is_int64()) return static_cast<double>(v->get_int64());
  if (v->is_uint64()) return static_cast<double>(v->get_uint64());
  throw_invalid_field(key);
}

double req_double(const bj::object& o, std::string_view key) {
  auto d = opt_double(o, key);
  if (!d) throw_invalid_field(key);
  return *d;
}

std::optional<bool> opt_bool(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return std::nullopt;
  if (!v->is_bool()) throw_invalid_field(key);
  return v->get_bool();
}

bool req_bool(const bj::object& o, std::string_view key) {
  auto b = opt_bool(o, key);
  if (!b) throw_invalid_field(key);
  return *b;
}

const bj::array* opt_array(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return nullptr;
  if (!v->is_array()) throw_invalid_field(key);
  return &v->get_array();
}

const bj::object* opt_object(const bj::object& o, std::string_view key) {
  const bj::value* v = find(o, key);
  if (!v) return nullptr;
  if (!v->is_object()) throw_invalid_field(key);
  return &v->get_object();
}

const bj::array& req_array(const bj::object& o, std::string_view key) {
  const bj::array* a = opt_array(o, key);
  if (!a) throw_invalid_field(key);
  return *a;
}

const bj::object& req_object(const bj::object& o, std::string_view key) {
  const bj::object* p = opt_object(o, key);
  if (!p) throw_invalid_field(key);
  return *p;
}

std::optional<std::vector<int64_t>> opt_int64_array(const bj::object& o, std::string_view key) {
  const bj::array* a = opt_array(o, key);
  if (!a) return std::nullopt;
  std::vector<int64_t> out;
  out.reserve(a->size());
  for (const auto& e : *a) {
    auto i = as_int64(e);
    if (!i) throw_invalid_field(key);
    out.push_back(*i);
  }
  return out;
}

std::vector<int64_t> req_int64_array(const bj::object& o, std::string_view key) {
  auto v = opt_int64_array(o, key);
  if (!v) throw_invalid_field(key);
  return std::move(*v);
}

std::optional<std::vector<std::string>> opt_string_array(const bj::object& o,
                                                         std::string_view key) {
  const bj::array* a = opt_array(o, key);
  if (!a) return std::nullopt;
  std::vector<std::string> out;
  out.reserve(a->size());
  for (const auto& e : *a) {
    if (!e.is_string()) throw_invalid_field(key);
    out.emplace_back(e.get_string());
  }
  return out;
}

}  // namespace azm
