// Owner: WP-B (internal helpers; see internal.hpp)
#include "mail/internal.hpp"

#include "core/strings.hpp"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>

namespace azm::mail::detail {
namespace {

boost::json::value parse_quiet(std::string_view json) {
  boost::system::error_code ec;
  boost::json::value v = boost::json::parse(json, ec);
  if (ec) return nullptr;
  return v;
}

bool is_token_char(unsigned char c) {
  // RFC 2045 token: any CHAR except SPACE, CTLs and tspecials.
  if (c <= 0x20 || c >= 0x7f) return false;
  switch (c) {
    case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':':
    case '\\': case '"': case '/': case '[': case ']': case '?': case '=':
      return false;
    default:
      return true;
  }
}

}  // namespace

std::vector<Address> addresses_from_json(std::string_view json) {
  std::vector<Address> out;
  const boost::json::value v = parse_quiet(json);
  const auto* arr = v.if_array();
  if (arr == nullptr) return out;
  out.reserve(arr->size());
  for (const auto& el : *arr) {
    const auto* o = el.if_object();
    if (o == nullptr) continue;
    const auto* email = o->if_contains("email");
    if (email == nullptr || !email->is_string()) continue;
    Address a;
    a.email = std::string(email->as_string());
    if (const auto* name = o->if_contains("name"); name != nullptr && name->is_string())
      a.name = std::string(name->as_string());
    out.push_back(std::move(a));
  }
  return out;
}

std::string addresses_to_json(std::span<const Address> list) {
  boost::json::array arr;
  arr.reserve(list.size());
  for (const auto& a : list) {
    boost::json::object o;
    o["name"] = a.name;
    o["email"] = a.email;
    arr.emplace_back(std::move(o));
  }
  return boost::json::serialize(arr);
}

std::vector<std::string> strings_from_json(std::string_view json) {
  std::vector<std::string> out;
  const boost::json::value v = parse_quiet(json);
  if (const auto* arr = v.if_array()) {
    for (const auto& el : *arr)
      if (el.is_string()) out.emplace_back(el.as_string());
  }
  return out;
}

boost::json::array array_from_json(std::string_view json) {
  boost::json::value v = parse_quiet(json);
  if (auto* a = v.if_array()) return std::move(*a);
  return {};
}

boost::json::object object_from_json(std::string_view json) {
  boost::json::value v = parse_quiet(json);
  if (auto* o = v.if_object()) return std::move(*o);
  return {};
}

std::string utf8_prefix_chars(std::string_view s, std::size_t max_chars) {
  std::size_t i = 0, count = 0;
  while (i < s.size() && count < max_chars) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if (c >= 0xF0 && c <= 0xF4) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC2 && c <= 0xDF) len = 2;
    if (i + len > s.size()) len = 1;
    for (std::size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        len = 1;
        break;
      }
    }
    i += len;
    ++count;
  }
  return std::string(s.substr(0, i));
}

std::size_t unicode_space_len(std::string_view s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 1;
  if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) return 2;
  if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
      static_cast<unsigned char>(s[i + 2]) == 0x80)
    return 3;
  return 0;
}

std::string collapse_whitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool pending = false;
  for (std::size_t i = 0; i < s.size();) {
    if (const std::size_t n = unicode_space_len(s, i); n > 0) {
      pending = !out.empty();
      i += n;
      continue;
    }
    if (pending) out.push_back(' ');
    pending = false;
    out.push_back(s[i]);
    ++i;
  }
  return out;
}

std::string like_contains(std::string_view s) {
  std::string out = "%";
  out.reserve(s.size() + 4);
  for (char c : s) {
    if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('%');
  return out;
}

std::string json_ids(std::span<const int64_t> ids) {
  std::string out = "[";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i) out.push_back(',');
    out += std::to_string(ids[i]);
  }
  out.push_back(']');
  return out;
}

std::string json_strings(std::span<const std::string> values) {
  boost::json::array arr;
  arr.reserve(values.size());
  for (const auto& v : values) arr.emplace_back(boost::json::string(v));
  return boost::json::serialize(arr);
}

std::set<std::string> owner_addresses(db::Conn& c, int64_t owner) {
  std::set<std::string> out;
  auto s = c.prepare(
      "SELECT email FROM addresses WHERE user_id = ? "
      "UNION SELECT a.email FROM addresses a JOIN alias_members am ON am.alias_id = a.id "
      "WHERE am.user_id = ?");
  s.bind_all(owner, owner);
  while (s.step()) out.insert(normalize_email(s.text(0)));
  return out;
}

std::string normalize_mime(std::string_view content_type) {
  std::string_view v = content_type;
  if (const auto semi = v.find(';'); semi != std::string_view::npos) v = v.substr(0, semi);
  v = trim(v);
  const auto slash = v.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= v.size()) return {};
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i == slash) continue;
    if (!is_token_char(static_cast<unsigned char>(v[i]))) return {};
  }
  if (v.size() > 255) return {};
  return to_lower_ascii(v);
}

}  // namespace azm::mail::detail
