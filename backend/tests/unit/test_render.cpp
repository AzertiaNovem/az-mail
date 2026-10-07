// Owner: WP-B — cid <-> signed URL rewriting, freeze stripping, serve policy, attachment views.
#include "core/signed_url.hpp"
#include "core/strings.hpp"
#include "mail/render.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace azm;
using namespace azm::mail;

namespace {

const std::string kBase = "https://api.mail.example";
const SignedUrls& urls() {
  static const SignedUrls u(std::string(test::kTestSecret), kBase);
  return u;
}
constexpr int64_t kUser = 7;
constexpr int64_t kExp = 1'800'000'000'000;

std::string signed_i(int64_t id) { return urls().file_url(id, kUser, 'i', kExp); }
std::string escaped(std::string s) { return replace_all(s, "&", "&amp;"); }

const std::vector<CidTarget> kTargets = {{11, "logo@azmail"}, {12, "img2@x"}, {13, "Pic%20One@x"}};

bool contains(std::string_view h, std::string_view s) { return h.find(s) != std::string_view::npos; }

}  // namespace

TEST_CASE("rewrite_cid_to_signed: src, href, background, srcset, style", "[render]") {
  const std::string in =
      "<p>x</p><img src=\"cid:logo@azmail\" alt=\"logo\">"
      "<a href='CID:img2@x'>link</a>"
      "<td background=cid:logo@azmail>t</td>"
      "<img srcset=\"cid:img2@x 1x, https://ext.example/a.png 2x\">"
      "<div style=\"background-image:url('cid:logo@azmail')\">s</div>";
  const std::string out = rewrite_cid_to_signed(in, kTargets, urls(), kUser, kExp);
  CHECK(contains(out, "<img src=\"" + escaped(signed_i(11)) + "\" alt=\"logo\" data-att-id=\"11\">"));
  CHECK(contains(out, "<a href=\"" + escaped(signed_i(12)) + "\">link</a>"));
  CHECK(contains(out, "<td background=\"" + escaped(signed_i(11)) + "\">"));
  CHECK(contains(out, "srcset=\"" + escaped(signed_i(12)) + " 1x, https://ext.example/a.png 2x\""));
  CHECK(contains(out, "url(" + escaped(signed_i(11)) + ")"));
  CHECK_FALSE(contains(out, "cid:"));
  CHECK(contains(out, "<p>x</p>"));
}

TEST_CASE("rewrite_cid_to_signed: edge cases", "[render]") {
  // Unknown cid untouched; angle brackets and percent-encoding accepted.
  CHECK(rewrite_cid_to_signed("<img src=\"cid:unknown@x\">", kTargets, urls(), kUser, kExp) ==
        "<img src=\"cid:unknown@x\">");
  CHECK(contains(rewrite_cid_to_signed("<img src=\"cid:<logo@azmail>\">", kTargets, urls(), kUser, kExp),
                 escaped(signed_i(11))));
  CHECK(contains(rewrite_cid_to_signed("<img src=\"cid:Pic%2520One@x\">", kTargets, urls(), kUser, kExp),
                 escaped(signed_i(13))));
  // Existing data-att-id is not duplicated.
  const std::string with_id =
      rewrite_cid_to_signed("<img data-att-id=\"11\" src=\"cid:logo@azmail\" />", kTargets, urls(), kUser, kExp);
  CHECK(with_id == "<img data-att-id=\"11\" src=\"" + escaped(signed_i(11)) + "\" />");
  // Self-closing tag keeps its form and gets the attribute before "/>".
  CHECK(rewrite_cid_to_signed("<img src=cid:img2@x />", kTargets, urls(), kUser, kExp) ==
        "<img src=\"" + escaped(signed_i(12)) + "\" data-att-id=\"12\"/>");
  // HTML rule: an unquoted value runs up to whitespace or '>', so "x/>" keeps the '/'.
  CHECK(rewrite_cid_to_signed("<img src=cid:img2@x/>", kTargets, urls(), kUser, kExp) ==
        "<img src=cid:img2@x/>");
  // Comments, <script> and <style> contents are left alone; no targets = identity.
  const std::string inert = "<!-- <img src=\"cid:logo@azmail\"> --><script>var a='<img src=cid:logo@azmail>'</script>";
  CHECK(rewrite_cid_to_signed(inert, kTargets, urls(), kUser, kExp) == inert);
  CHECK(rewrite_cid_to_signed("<img src=\"cid:logo@azmail\">", {}, urls(), kUser, kExp) ==
        "<img src=\"cid:logo@azmail\">");
  CHECK(rewrite_cid_to_signed("no html here", kTargets, urls(), kUser, kExp) == "no html here");
  // Malformed trailing tag is copied verbatim.
  CHECK(rewrite_cid_to_signed("ok <img src=\"cid:logo@azmail", kTargets, urls(), kUser, kExp) ==
        "ok <img src=\"cid:logo@azmail");
}

TEST_CASE("rewrite_signed_to_cid: data-att-id and file URLs", "[render]") {
  // Client HTML as the editor produces it: data-att-id + signed view URL.
  const std::string in = "<p>hi</p><img data-att-id=\"11\" src=\"" + escaped(signed_i(11)) +
                         "\"><a href=\"" + escaped(urls().file_url(12, kUser, 'a', kExp)) + "\">dl</a>";
  const std::string out = rewrite_signed_to_cid(in, kTargets, kBase);
  CHECK(out == "<p>hi</p><img data-att-id=\"11\" src=\"cid:logo@azmail\"><a href=\"cid:img2@x\">dl</a>");

  // data-att-id wins even when src points elsewhere / is missing.
  CHECK(rewrite_signed_to_cid("<img data-att-id=12 src=\"blob:abc\">", kTargets, kBase) ==
        "<img data-att-id=12 src=\"cid:img2@x\">");
  CHECK(rewrite_signed_to_cid("<img data-att-id=\"12\">", kTargets, kBase) ==
        "<img data-att-id=\"12\" src=\"cid:img2@x\">");
  // Unknown ids (another user's attachment) stay as they are.
  const std::string foreign = "<img src=\"" + kBase + "/api/files/999?d=i&amp;u=1&amp;exp=1&amp;sig=x\">";
  CHECK(rewrite_signed_to_cid(foreign, kTargets, kBase) == foreign);
  CHECK(rewrite_signed_to_cid("<img data-att-id=\"999\" src=\"x\">", kTargets, kBase) ==
        "<img data-att-id=\"999\" src=\"x\">");
  // Host compared case-insensitively; relative URLs accepted; raw URLs and other paths not.
  CHECK(rewrite_signed_to_cid("<img src=\"HTTPS://API.MAIL.EXAMPLE/api/files/11?d=i\">", kTargets, kBase) ==
        "<img src=\"cid:logo@azmail\">");
  CHECK(rewrite_signed_to_cid("<img src=\"/api/files/12?d=i\">", kTargets, kBase) == "<img src=\"cid:img2@x\">");
  CHECK(rewrite_signed_to_cid("<a href=\"" + kBase + "/api/files/raw/11?u=1\">r</a>", kTargets, kBase) ==
        "<a href=\"" + kBase + "/api/files/raw/11?u=1\">r</a>");
  CHECK(rewrite_signed_to_cid("<img src=\"https://evil.example/api/files/11?d=i\">", kTargets, kBase) ==
        "<img src=\"https://evil.example/api/files/11?d=i\">");
  CHECK(rewrite_signed_to_cid("<img src=\"" + kBase + "/api/files/11x\">", kTargets, kBase) ==
        "<img src=\"" + kBase + "/api/files/11x\">");
  // Trailing slash on the configured base is tolerated.
  CHECK(rewrite_signed_to_cid("<img src=\"" + kBase + "/api/files/11\">", kTargets, kBase + "/") ==
        "<img src=\"cid:logo@azmail\">");
}

TEST_CASE("cid round trip keeps the stored-HTML invariant", "[render]") {
  const std::string stored = "<div><img src=\"cid:logo@azmail\" data-att-id=\"11\" width=10>text</div>";
  const std::string shown = rewrite_cid_to_signed(stored, kTargets, urls(), kUser, kExp);
  CHECK_FALSE(contains(shown, "cid:"));
  CHECK(rewrite_signed_to_cid(shown, kTargets, kBase) == stored);
}

TEST_CASE("strip_att_ids", "[render]") {
  CHECK(strip_att_ids("<img src=\"cid:a\" data-att-id=\"1\">") == "<img src=\"cid:a\">");
  CHECK(strip_att_ids("<img data-att-id=1 src=cid:a>") == "<img src=cid:a>");
  CHECK(strip_att_ids("<IMG DATA-ATT-ID='2'/>") == "<IMG/>");
  CHECK(strip_att_ids("<p>plain</p>") == "<p>plain</p>");
  CHECK(strip_att_ids("<p>data-att-id=\"1\" as text</p>") == "<p>data-att-id=\"1\" as text</p>");
}

TEST_CASE("strip_api_file_urls", "[render]") {
  const std::string signed_url = escaped(signed_i(11));
  CHECK(strip_api_file_urls("<img src=\"" + signed_url + "\" alt=x>", kBase) == "<img alt=x>");
  CHECK(strip_api_file_urls("<a href=\"" + signed_url + "\">a</a>", kBase) == "<a>a</a>");
  CHECK(strip_api_file_urls("<img srcset=\"x.png 1x, " + signed_url + " 2x\">", kBase) == "<img>");
  CHECK(strip_api_file_urls("<video poster=\"" + signed_url + "\"></video>", kBase) == "<video></video>");
  CHECK(strip_api_file_urls("<td background=\"" + signed_url + "\">", kBase) == "<td>");
  // Entity-encoded and case variants are caught.
  CHECK(strip_api_file_urls("<img src=\"https&#58;//API.mail.example/api/files/1?x\">", kBase) == "<img>");
  CHECK(strip_api_file_urls("<img src=\"http://api.mail.example/api/files/1\">", kBase) == "<img>");
  // CSS in style attributes and <style> blocks.
  CHECK(strip_api_file_urls("<div style=\"background:url('" + kBase + "/api/files/3?a')\">", kBase) ==
        "<div style=\"background:url(about:blank)\">");
  CHECK(strip_api_file_urls("<style>.a{background:url(" + kBase + "/api/files/3)}</style>", kBase) ==
        "<style>.a{background:url(about:blank)}</style>");
  // cid: and foreign URLs survive.
  const std::string keep = "<img src=\"cid:a@b\"><a href=\"https://other.example/api/files/1\">x</a>";
  CHECK(strip_api_file_urls(keep, kBase) == keep);
}

TEST_CASE("serve policy and inline-safe types", "[render]") {
  for (std::string_view t : {"image/png", "image/jpeg", "image/gif", "image/webp", "image/avif", "image/bmp",
                             "application/pdf", "IMAGE/PNG", "image/jpeg; name=x.jpg"})
    CHECK(is_inline_safe_type(t));
  for (std::string_view t : {"image/svg+xml", "text/html", "text/plain", "application/octet-stream", "",
                             "image", "application/javascript", "image/x-icon"})
    CHECK_FALSE(is_inline_safe_type(t));

  auto p = file_serve_policy("image/png", "图.png", 'i');
  CHECK(p.content_type == "image/png");
  CHECK(p.redirect_content_type == "image/png");
  CHECK(p.disposition == content_disposition("inline", "图.png"));
  CHECK(istarts_with(p.disposition, "inline;"));
  CHECK(contains(p.disposition, "filename*=UTF-8''%E5%9B%BE.png"));
  CHECK_FALSE(p.sandbox_csp);

  p = file_serve_policy("image/png", "a.png", 'a');
  CHECK(istarts_with(p.disposition, "attachment;"));
  CHECK_FALSE(p.sandbox_csp);

  p = file_serve_policy("application/pdf", "报告.pdf", 'i');
  CHECK(istarts_with(p.disposition, "inline;"));
  CHECK_FALSE(p.sandbox_csp);  // Chrome's PDF viewer breaks under a sandbox (D4)

  p = file_serve_policy("image/svg+xml", "x.svg", 'i');
  CHECK(istarts_with(p.disposition, "attachment;"));
  CHECK(p.sandbox_csp);
  CHECK(p.content_type == "image/svg+xml");
  CHECK(p.redirect_content_type == "application/octet-stream");

  p = file_serve_policy("text/html; charset=utf-8", "x.html", 'i');
  CHECK(istarts_with(p.disposition, "attachment;"));
  CHECK(p.sandbox_csp);
  CHECK(p.content_type == "text/html");

  p = file_serve_policy("", "noname", 'a');
  CHECK(p.content_type == "application/octet-stream");
  p = file_serve_policy("bad type\r\nX-Injected: 1", "x", 'a');
  CHECK(p.content_type == "application/octet-stream");
}

TEST_CASE("make_attachment_view", "[render]") {
  AttachmentRecord a;
  a.id = 5;
  a.owner_id = kUser;
  a.filename = "a.png";
  a.content_type = "image/png";
  a.size = 10;
  a.content_id = "c@x";
  a.is_inline = true;
  auto v = make_attachment_view(a, urls(), kUser, kExp);
  CHECK(v.id == 5);
  CHECK(v.is_inline);
  CHECK(v.content_id == "c@x");
  CHECK(v.download_url == urls().file_url(5, kUser, 'a', kExp));
  CHECK(v.view_url == urls().file_url(5, kUser, 'i', kExp));
  CHECK(urls().verify_file(5, kUser, 'a', kExp, urls().sign_file(5, kUser, 'a', kExp), kExp - 1));

  a.content_type = "application/zip";
  v = make_attachment_view(a, urls(), kUser, kExp);
  CHECK_FALSE(v.view_url.has_value());
}
