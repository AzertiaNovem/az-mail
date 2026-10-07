// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements (unit-tested against the AWS vectors in
// DESIGN A.2).
#include "storage/s3_sigv4.hpp"

#include "core/errors.hpp"

namespace azm::storage::sigv4 {

std::string uri_encode(std::string_view, bool) { throw NotImplemented("sigv4::uri_encode"); }
std::string canonical_uri(std::string_view) { throw NotImplemented("sigv4::canonical_uri"); }
std::string canonical_query(std::span<const std::pair<std::string, std::string>>) {
  throw NotImplemented("sigv4::canonical_query");
}
CanonicalHeaders canonical_headers(std::span<const std::pair<std::string, std::string>>) {
  throw NotImplemented("sigv4::canonical_headers");
}
std::string canonical_request(std::string_view, std::string_view, std::string_view,
                              const CanonicalHeaders&, std::string_view) {
  throw NotImplemented("sigv4::canonical_request");
}
std::string credential_scope(std::string_view, std::string_view, std::string_view) {
  throw NotImplemented("sigv4::credential_scope");
}
std::string string_to_sign(std::string_view, std::string_view, std::string_view) {
  throw NotImplemented("sigv4::string_to_sign");
}
std::string signing_key(std::string_view, std::string_view, std::string_view, std::string_view) {
  throw NotImplemented("sigv4::signing_key");
}
std::string signature(std::string_view, std::string_view) {
  throw NotImplemented("sigv4::signature");
}
std::string authorization_header(std::string_view, std::string_view, std::string_view,
                                 std::string_view) {
  throw NotImplemented("sigv4::authorization_header");
}
std::string amz_date(int64_t) { throw NotImplemented("sigv4::amz_date"); }
SignedRequest sign_request(const Credentials&, const RequestToSign&) {
  throw NotImplemented("sigv4::sign_request");
}
PresignedUrl presign(const Credentials&, const PresignInput&) {
  throw NotImplemented("sigv4::presign");
}

}  // namespace azm::storage::sigv4
