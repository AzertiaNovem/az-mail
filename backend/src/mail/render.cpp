// Owner: WP-B
// cid: <-> signed URL rewriting and file-serving policy (render.hpp; C10, D4, D5).
#include "mail/render.hpp"

#include "core/strings.hpp"
#include "mail/html_scan.hpp"
#include "mail/internal.hpp"

#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace azm::mail {
namespace {

using html::Attr;
using html::MarkupKind;
using html::Tag;

constexpr std::string_view kOctetStream = "application/octet-stream";

// URL-bearing attributes we rewrite (srcset is a candidate list).
bool is_url_attr(std::string_view n) {
  return n == "src" || n == "href" || n == "background" || n == "srcset" || n == "poster";
}

// One edit on a tag: replace an attribute (or remove it with an empty replacement), and
// attributes appended before the tag's end.
struct TagEdit {
  std::vector<std::pair<std::size_t, std::string>> replace;  // attr index → raw text ("" = drop)
  std::vector<std::string> append;                           // raw attribute text
  bool empty() const { return replace.empty() && append.empty(); }
};

void emit_tag(std::string& out, std::string_view h, const Tag& t, const TagEdit& e) {
  std::size_t pos = t.begin;
  for (std::size_t k = 0; k < t.attrs.size(); ++k) {
    const Attr& a = t.attrs[k];
    const std::string* rep = nullptr;
    for (const auto& [idx, text] : e.replace)
      if (idx == k) rep = &text;
    if (rep == nullptr) continue;
    if (rep->empty()) {
      // Drop the attribute together with the whitespace before it.
      std::size_t ws = a.begin;
      while (ws > pos && (h[ws - 1] == ' ' || h[ws - 1] == '\t' || h[ws - 1] == '\n' ||
                          h[ws - 1] == '\r' || h[ws - 1] == '\f'))
        --ws;
      out.append(h.substr(pos, ws - pos));
    } else {
      out.append(h.substr(pos, a.begin - pos));
      out.append(*rep);
    }
    pos = a.end;
  }
  out.append(h.substr(pos, t.close_at - pos));
  for (const auto& extra : e.append) {
    if (!out.empty() && out.back() != ' ') out.push_back(' ');
    out.append(extra);
  }
  out.append(h.substr(t.close_at, t.end - t.close_at));
}

std::string attr_text(std::string_view name, std::string_view value) {
  return std::string(name) + "=\"" + html::escape_attr(value) + "\"";
}

// Walks every complete start tag outside comments and raw-text elements; `fn` returns the
// edit for a tag. <style> contents go to `style_fn` (may be null) for CSS rewriting.
std::string rewrite_tags(std::string_view h, const std::function<TagEdit(const Tag&)>& fn,
                         const std::function<std::string(std::string_view)>& style_fn = {}) {
  std::string out;
  out.reserve(h.size() + h.size() / 8);
  std::size_t copied = 0, i = 0;
  while (i < h.size()) {
    const std::size_t lt = h.find('<', i);
    if (lt == std::string_view::npos) break;
    auto m = html::parse_markup(h, lt);
    if (!m) {
      i = lt + 1;
      continue;
    }
    if (m->kind != MarkupKind::Tag || m->tag.closing || !m->tag.complete) {
      i = m->end;
      continue;
    }
    const Tag& t = m->tag;
    TagEdit e = fn(t);
    if (!e.empty()) {
      out.append(h.substr(copied, t.begin - copied));
      emit_tag(out, h, t, e);
      copied = t.end;
    }
    i = m->end;
    if (html::is_raw_text_element(t.name) && !t.self_closing) {
      const std::size_t close = html::find_close_tag(h, i, t.name);
      const std::size_t content_end = close == std::string_view::npos ? h.size() : close;
      if (t.name == "style" && style_fn) {
        out.append(h.substr(copied, i - copied));
        out.append(style_fn(h.substr(i, content_end - i)));
        copied = content_end;
      }
      i = content_end;
    }
  }
  out.append(h.substr(copied));
  return out;
}

// "cid:<x>" / "CID:x" → "x" (also percent-decoded per RFC 2392); nullopt for other URLs.
std::optional<std::string> cid_of(std::string_view url) {
  url = trim(url);
  if (!istarts_with(url, "cid:")) return std::nullopt;
  std::string_view id = trim(url.substr(4));
  if (id.size() >= 2 && id.front() == '<' && id.back() == '>') id = id.substr(1, id.size() - 2);
  if (id.empty()) return std::nullopt;
  if (auto dec = url_decode(id); dec && !dec->empty()) return *dec;
  return std::string(id);
}

const CidTarget* find_cid(std::span<const CidTarget> targets, std::string_view cid) {
  for (const auto& t : targets)
    if (t.content_id == cid) return &t;
  for (const auto& t : targets)  // Content-IDs are case-sensitive, but clients do vary
    if (iequals(t.content_id, cid)) return &t;
  return nullptr;
}

const CidTarget* find_id(std::span<const CidTarget> targets, int64_t id) {
  for (const auto& t : targets)
    if (t.attachment_id == id) return &t;
  return nullptr;
}

std::optional<int64_t> parse_id(std::string_view s) {
  if (s.empty() || s.size() > 18) return std::nullopt;
  int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v <= 0) return std::nullopt;
  return v;
}

std::string strip_trailing_slashes(std::string_view s) {
  while (!s.empty() && s.back() == '/') s.remove_suffix(1);
  return std::string(s);
}

// Attachment id of "<base>/api/files/<id>[?…|#…]" (or the relative "/api/files/<id>…"),
// scheme/host compared case-insensitively. Raw-message URLs (/api/files/raw/…) don't match.
std::optional<int64_t> api_file_id(std::string_view url, std::string_view base) {
  url = trim(url);
  std::string_view rest;
  if (!base.empty() && istarts_with(url, base)) rest = url.substr(base.size());
  else if (url.substr(0, 1) == "/" && url.substr(0, 2) != "//") rest = url;
  else return std::nullopt;
  constexpr std::string_view kPath = "/api/files/";
  if (!istarts_with(rest, kPath)) return std::nullopt;
  rest.remove_prefix(kPath.size());
  std::size_t end = 0;
  while (end < rest.size() && rest[end] >= '0' && rest[end] <= '9') ++end;
  if (end < rest.size() && rest[end] != '?' && rest[end] != '#') return std::nullopt;
  return parse_id(rest.substr(0, end));
}

// Rewrites each URL of a srcset candidate list through `fn` (nullopt = keep).
std::optional<std::string> rewrite_srcset(std::string_view v,
                                          const std::function<std::optional<std::string>(std::string_view)>& fn) {
  bool changed = false;
  std::string out;
  for (const auto& cand : split(v, ',')) {
    std::string_view c = trim(cand);
    if (c.empty()) continue;
    std::string_view url = c, desc;
    if (const auto sp = c.find_first_of(" \t\n"); sp != std::string_view::npos) {
      url = c.substr(0, sp);
      desc = trim(c.substr(sp));
    }
    std::string u(url);
    if (auto r = fn(url)) {
      u = *r;
      changed = true;
    }
    if (!out.empty()) out += ", ";
    out += u;
    if (!desc.empty()) {
      out.push_back(' ');
      out.append(desc);
    }
  }
  if (!changed) return std::nullopt;
  return out;
}

// Rewrites CSS url(...) values through `fn` (nullopt = keep). Handles quotes and whitespace.
std::optional<std::string> rewrite_css_urls(std::string_view css,
                                            const std::function<std::optional<std::string>(std::string_view)>& fn) {
  std::string out;
  bool changed = false;
  std::size_t pos = 0, copied = 0;
  while (pos < css.size()) {
    std::size_t u = std::string_view::npos;
    for (std::size_t k = pos; k + 4 <= css.size(); ++k) {
      if ((css[k] == 'u' || css[k] == 'U') && iequals(css.substr(k, 4), "url(")) {
        u = k;
        break;
      }
    }
    if (u == std::string_view::npos) break;
    std::size_t vb = u + 4;
    while (vb < css.size() && (css[vb] == ' ' || css[vb] == '\t' || css[vb] == '\n')) ++vb;
    char q = 0;
    if (vb < css.size() && (css[vb] == '"' || css[vb] == '\'')) q = css[vb++];
    std::size_t ve = q ? css.find(q, vb) : css.find(')', vb);
    if (ve == std::string_view::npos) break;
    std::string_view value = css.substr(vb, ve - vb);
    if (!q) value = trim(value);
    std::size_t close = css.find(')', q ? ve + 1 : ve);
    if (close == std::string_view::npos) break;
    if (auto r = fn(value)) {
      out.append(css.substr(copied, u - copied));
      out += "url(" + *r + ")";
      copied = close + 1;
      changed = true;
    }
    pos = close + 1;
  }
  if (!changed) return std::nullopt;
  out.append(css.substr(copied));
  return out;
}

// Case-insensitive "contains <base>/api/files/" (base without trailing slash).
bool mentions_api_files(std::string_view value, std::string_view base) {
  const std::string v = to_lower_ascii(value);
  const std::string needle = to_lower_ascii(std::string(base) + "/api/files/");
  if (v.find(needle) != std::string::npos) return true;
  // A scheme-relative or differently-cased form of the same host is caught by the lowercase
  // compare; also catch "//host/api/files/" when the base has a scheme.
  if (const auto p = needle.find("//"); p != std::string::npos && v.find(needle.substr(p)) != std::string::npos)
    return true;
  return false;
}

}  // namespace

std::string rewrite_cid_to_signed(std::string_view h, std::span<const CidTarget> targets,
                                  const SignedUrls& urls, int64_t user_id, int64_t exp_ms) {
  if (targets.empty() || to_lower_ascii(h).find("cid:") == std::string::npos) return std::string(h);
  // `url` is an already entity-decoded attribute / CSS value.
  auto signed_for = [&](std::string_view url) -> std::optional<std::string> {
    const auto cid = cid_of(url);
    if (!cid) return std::nullopt;
    const CidTarget* t = find_cid(targets, *cid);
    if (t == nullptr) return std::nullopt;
    return urls.file_url(t->attachment_id, user_id, 'i', exp_ms);
  };
  return rewrite_tags(h, [&](const Tag& t) {
    TagEdit e;
    const CidTarget* img_target = nullptr;
    for (std::size_t k = 0; k < t.attrs.size(); ++k) {
      const Attr& a = t.attrs[k];
      if (!a.has_value) continue;
      const std::string_view raw = a.raw_value(h);
      if (a.name == "srcset") {
        if (auto r = rewrite_srcset(html::decode_entities(raw), signed_for))
          e.replace.emplace_back(k, attr_text(a.name, *r));
      } else if (is_url_attr(a.name)) {
        const std::string decoded = html::decode_entities(raw);
        if (auto r = signed_for(decoded)) {
          e.replace.emplace_back(k, attr_text(a.name, *r));
          if (a.name == "src" && t.name == "img") img_target = find_cid(targets, *cid_of(decoded));
        }
      } else if (a.name == "style") {
        if (auto r = rewrite_css_urls(html::decode_entities(raw), signed_for))
          e.replace.emplace_back(k, attr_text(a.name, *r));
      }
    }
    if (img_target != nullptr && t.find("data-att-id") == nullptr)
      e.append.push_back(attr_text("data-att-id", std::to_string(img_target->attachment_id)));
    return e;
  });
}

std::string rewrite_signed_to_cid(std::string_view h, std::span<const CidTarget> targets,
                                  std::string_view api_base_url) {
  if (targets.empty()) return std::string(h);
  const std::string base = strip_trailing_slashes(trim(api_base_url));
  // `url` is an already entity-decoded attribute / CSS value.
  auto cid_for = [&](std::string_view url) -> std::optional<std::string> {
    const auto id = api_file_id(url, base);
    if (!id) return std::nullopt;
    const CidTarget* t = find_id(targets, *id);
    if (t == nullptr) return std::nullopt;
    return "cid:" + t->content_id;
  };
  return rewrite_tags(h, [&](const Tag& t) {
    TagEdit e;
    const CidTarget* by_att = nullptr;
    if (t.name == "img") {
      if (const Attr* a = t.find("data-att-id"); a && a->has_value)
        if (auto id = parse_id(trim(html::decode_entities(a->raw_value(h))))) by_att = find_id(targets, *id);
    }
    bool src_seen = false;
    for (std::size_t k = 0; k < t.attrs.size(); ++k) {
      const Attr& a = t.attrs[k];
      if (a.name == "src" && by_att != nullptr) {
        if (!src_seen) e.replace.emplace_back(k, attr_text("src", "cid:" + by_att->content_id));
        else e.replace.emplace_back(k, std::string());  // duplicate src attributes
        src_seen = true;
        continue;
      }
      if (!a.has_value) continue;
      const std::string_view raw = a.raw_value(h);
      if (a.name == "srcset") {
        if (auto r = rewrite_srcset(html::decode_entities(raw), cid_for))
          e.replace.emplace_back(k, attr_text(a.name, *r));
      } else if (is_url_attr(a.name)) {
        if (auto r = cid_for(html::decode_entities(raw))) e.replace.emplace_back(k, attr_text(a.name, *r));
      } else if (a.name == "style") {
        if (auto r = rewrite_css_urls(html::decode_entities(raw), cid_for))
          e.replace.emplace_back(k, attr_text(a.name, *r));
      }
    }
    if (by_att != nullptr && !src_seen) e.append.push_back(attr_text("src", "cid:" + by_att->content_id));
    return e;
  });
}

std::string strip_att_ids(std::string_view h) {
  if (to_lower_ascii(h).find("data-att-id") == std::string::npos) return std::string(h);
  return rewrite_tags(h, [&](const Tag& t) {
    TagEdit e;
    for (std::size_t k = 0; k < t.attrs.size(); ++k)
      if (t.attrs[k].name == "data-att-id") e.replace.emplace_back(k, std::string());
    return e;
  });
}

std::string strip_api_file_urls(std::string_view h, std::string_view api_base_url) {
  const std::string base = strip_trailing_slashes(trim(api_base_url));
  auto blank_css = [&](std::string_view url) -> std::optional<std::string> {
    if (mentions_api_files(url, base)) return std::string("about:blank");
    return std::nullopt;
  };
  return rewrite_tags(
      h,
      [&](const Tag& t) {
        TagEdit e;
        for (std::size_t k = 0; k < t.attrs.size(); ++k) {
          const Attr& a = t.attrs[k];
          if (!a.has_value) continue;
          const std::string decoded = html::decode_entities(a.raw_value(h));
          if (is_url_attr(a.name)) {
            if (mentions_api_files(decoded, base)) e.replace.emplace_back(k, std::string());
          } else if (a.name == "style") {
            if (auto r = rewrite_css_urls(decoded, blank_css)) e.replace.emplace_back(k, attr_text("style", *r));
          }
        }
        return e;
      },
      [&](std::string_view css) {
        if (auto r = rewrite_css_urls(css, blank_css)) return *r;
        return std::string(css);
      });
}

bool is_inline_safe_type(std::string_view content_type) {
  const std::string t = detail::normalize_mime(content_type);
  return t == "image/png" || t == "image/jpeg" || t == "image/gif" || t == "image/webp" ||
         t == "image/avif" || t == "image/bmp" || t == "application/pdf";
}

FileServePolicy file_serve_policy(std::string_view content_type, std::string_view filename, char d) {
  FileServePolicy p;
  std::string t = detail::normalize_mime(content_type);
  if (t.empty()) t = std::string(kOctetStream);
  const bool safe = is_inline_safe_type(t);
  p.content_type = t;
  p.redirect_content_type = safe ? t : std::string(kOctetStream);
  p.disposition = content_disposition(d == 'i' && safe ? "inline" : "attachment", filename);
  p.sandbox_csp = !safe;
  return p;
}

AttachmentView make_attachment_view(const AttachmentRecord& a, const SignedUrls& urls,
                                    int64_t user_id, int64_t exp_ms) {
  AttachmentView v;
  v.id = a.id;
  v.filename = a.filename;
  v.content_type = a.content_type;
  v.size = a.size;
  v.is_inline = a.is_inline;
  v.content_id = a.content_id;
  v.download_url = urls.file_url(a.id, user_id, 'a', exp_ms);
  if (is_inline_safe_type(a.content_type)) v.view_url = urls.file_url(a.id, user_id, 'i', exp_ms);
  return v;
}

}  // namespace azm::mail
