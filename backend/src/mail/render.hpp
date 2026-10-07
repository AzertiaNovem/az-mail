// Owner: WP-B
//
// Read-time rendering helpers (DESIGN C10 stored-HTML invariant, D4 file serving, D5 signed URLs).
// Pure functions (no DB): stored HTML references local attachments ONLY as cid:<content_id>
// (images additionally carry data-att-id="<attachment id>"); signed URLs exist only in API
// responses and are rewritten back to cid: whenever a client sends HTML.
#pragma once

#include "core/signed_url.hpp"
#include "mail/types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace azm::mail {

// An attachment that may be referenced from HTML.
struct CidTarget {
  int64_t attachment_id = 0;
  std::string content_id;  // without <>
};

// Replaces src/href/background values "cid:<content_id>" (case-insensitive scheme, optional <>)
// of known targets with signed inline URLs (SignedUrls::file_url(id, user_id, 'i', exp_ms)) and
// adds data-att-id="<id>" to <img> tags lacking it. Unknown cids are left untouched.
// Callers compute exp_ms = urls.expiry(now) (cfg.signed_url_ttl_sec, core/signed_url.hpp).
std::string rewrite_cid_to_signed(std::string_view html, std::span<const CidTarget> targets,
                                  const SignedUrls& urls, int64_t user_id, int64_t exp_ms);

// Inverse for client-supplied HTML: any <img> with data-att-id="<id>" of a known target gets
// src="cid:<content_id>"; any remaining src/href pointing at "<api_base_url>/api/files/<id>?…"
// of a known target becomes cid:<content_id>. Other URLs are untouched.
std::string rewrite_signed_to_cid(std::string_view html, std::span<const CidTarget> targets,
                                  std::string_view api_base_url);

// Send freeze: removes every data-att-id attribute (cid: references stay).
std::string strip_att_ids(std::string_view html);

// Send freeze, after strip_att_ids: removes every remaining reference to our own file
// endpoints, so signed URLs never leave the server (they would leak the sender's 12 h
// capability and break after expiry). Any src / href / srcset / background / poster attribute
// whose value contains "<api_base_url>/api/files/" (case-insensitive scheme/host, also when
// HTML-entity encoded) is removed; CSS url(...) values pointing there (style attributes and
// <style> blocks) become url(about:blank). cid: and foreign URLs are untouched. Pure.
std::string strip_api_file_urls(std::string_view html, std::string_view api_base_url);

// Content types that may be shown inline from the API origin (D4): image/png, image/jpeg,
// image/gif, image/webp, image/avif, image/bmp and application/pdf (parameters ignored,
// case-insensitive). image/svg+xml and text/html are never inline-safe.
bool is_inline_safe_type(std::string_view content_type);

// How GET /api/files/:id must serve an attachment (D4 / Addendum A).
struct FileServePolicy {
  std::string content_type;           // header for local/proxy delivery (original type, or
                                      // application/octet-stream when empty/invalid)
  std::string redirect_content_type;  // response-content-type for R2 redirect: original when
                                      // inline-safe, else application/octet-stream
  std::string disposition;            // RFC 6266 value (core::content_disposition): "inline"
                                      // only when d == 'i' and inline-safe, else "attachment"
  bool sandbox_csp = false;           // add "Content-Security-Policy: sandbox" (not inline-safe)
};
FileServePolicy file_serve_policy(std::string_view content_type, std::string_view filename, char d);

// API Attachment view: download_url = signed d='a'; view_url = signed d='i' only when
// is_inline_safe_type(content_type), else null.
AttachmentView make_attachment_view(const AttachmentRecord& a, const SignedUrls& urls,
                                    int64_t user_id, int64_t exp_ms);

}  // namespace azm::mail
