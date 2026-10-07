// Owner: WP-A
// Pattern router (see router.hpp for the matching rules). Routes are kept in registration order
// with their patterns pre-split into segments; with ~60 routes a linear scan is faster than a
// trie and keeps "literal beats :param at each position" trivially correct.
#include "http/router.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace azm::http {

namespace {

struct Segment {
  std::string text;  // literal text, or the parameter name for params
  bool param = false;
};

// Splits "/a/b/c" into {"a","b","c"}. Returns false for an empty segment ("//", trailing '/')
// unless the whole path is "/".
bool split_path(std::string_view path, std::vector<std::string_view>& out) {
  out.clear();
  if (path.empty() || path.front() != '/') return false;
  if (path == "/") return true;
  std::size_t pos = 1;
  for (;;) {
    const std::size_t next = path.find('/', pos);
    const std::string_view seg =
        path.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    if (seg.empty()) return false;
    out.push_back(seg);
    if (next == std::string_view::npos) return true;
    pos = next + 1;
  }
}

}  // namespace

struct Router::Impl {
  struct Entry {
    Route route;
    std::vector<Segment> segments;
  };
  std::vector<Entry> entries;  // registration order (stable addresses not required: we hand out
                               // pointers only after startup, when no more add() calls happen)

  // True when `e` matches `segs`; fills params when `params` is non-null.
  static bool matches(const Entry& e, const std::vector<std::string_view>& segs, Params* params) {
    if (e.segments.size() != segs.size()) return false;
    for (std::size_t i = 0; i < segs.size(); ++i) {
      const Segment& s = e.segments[i];
      if (!s.param && s.text != segs[i]) return false;
    }
    if (params) {
      params->clear();
      for (std::size_t i = 0; i < segs.size(); ++i)
        if (e.segments[i].param) params->insert_or_assign(e.segments[i].text, std::string(segs[i]));
    }
    return true;
  }

  // a is more specific than b: at the first position where they differ in kind, a has the literal.
  static bool more_specific(const Entry& a, const Entry& b) {
    for (std::size_t i = 0; i < a.segments.size() && i < b.segments.size(); ++i) {
      if (a.segments[i].param != b.segments[i].param) return !a.segments[i].param;
    }
    return false;
  }
};

Router::Router() : impl_(std::make_unique<Impl>()) {}
Router::~Router() = default;
Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

void Router::add(Route route) {
  std::vector<std::string_view> raw;
  if (!split_path(route.pattern, raw))
    throw std::invalid_argument("invalid route pattern: " + route.pattern);
  Impl::Entry e;
  e.segments.reserve(raw.size());
  for (std::string_view s : raw) {
    if (s.front() == ':') {
      if (s.size() == 1) throw std::invalid_argument("empty parameter name in route: " + route.pattern);
      e.segments.push_back({std::string(s.substr(1)), true});
    } else {
      e.segments.push_back({std::string(s), false});
    }
  }
  // Duplicate = same method and the same shape (parameter names do not matter for matching).
  for (const auto& other : impl_->entries) {
    if (other.route.method != route.method || other.segments.size() != e.segments.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < e.segments.size() && same; ++i) {
      const auto& a = other.segments[i];
      const auto& b = e.segments[i];
      same = a.param == b.param && (a.param || a.text == b.text);
    }
    if (same)
      throw std::invalid_argument("duplicate route: " + std::string(beast::http::to_string(route.method)) +
                                  " " + route.pattern);
  }
  if (!route.handler) throw std::invalid_argument("route without handler: " + route.pattern);
  e.route = std::move(route);
  impl_->entries.push_back(std::move(e));
}

Router::Match Router::match(beast::http::verb method, std::string_view path) const {
  Match m;
  std::vector<std::string_view> segs;
  if (!split_path(path, segs)) return m;
  const Impl::Entry* best = nullptr;
  for (const auto& e : impl_->entries) {
    if (!Impl::matches(e, segs, nullptr)) continue;
    if (e.route.method != method) {
      m.path_exists = true;
      continue;
    }
    if (best == nullptr || Impl::more_specific(e, *best)) best = &e;
  }
  if (best != nullptr) {
    Impl::matches(*best, segs, &m.params);
    m.route = &best->route;
    m.path_exists = true;
  }
  return m;
}

std::vector<beast::http::verb> Router::allowed_methods(std::string_view path) const {
  std::vector<beast::http::verb> out;
  std::vector<std::string_view> segs;
  if (!split_path(path, segs)) return out;
  for (const auto& e : impl_->entries) {
    if (!Impl::matches(e, segs, nullptr)) continue;
    if (std::find(out.begin(), out.end(), e.route.method) == out.end()) out.push_back(e.route.method);
  }
  return out;
}

std::size_t Router::size() const { return impl_->entries.size(); }

}  // namespace azm::http
