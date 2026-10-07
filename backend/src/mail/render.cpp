// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/render.hpp"

#include "core/errors.hpp"

namespace azm::mail {

std::string rewrite_cid_to_signed(std::string_view, std::span<const CidTarget>, const SignedUrls&,
                                  int64_t, int64_t) {
  throw NotImplemented("mail::rewrite_cid_to_signed");
}
std::string rewrite_signed_to_cid(std::string_view, std::span<const CidTarget>, std::string_view) {
  throw NotImplemented("mail::rewrite_signed_to_cid");
}
std::string strip_att_ids(std::string_view) { throw NotImplemented("mail::strip_att_ids"); }
std::string strip_api_file_urls(std::string_view, std::string_view) {
  throw NotImplemented("mail::strip_api_file_urls");
}
bool is_inline_safe_type(std::string_view) { throw NotImplemented("mail::is_inline_safe_type"); }
FileServePolicy file_serve_policy(std::string_view, std::string_view, char) {
  throw NotImplemented("mail::file_serve_policy");
}
AttachmentView make_attachment_view(const AttachmentRecord&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::make_attachment_view");
}

}  // namespace azm::mail
