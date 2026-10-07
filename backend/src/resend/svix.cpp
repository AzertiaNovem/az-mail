// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements verification.
#include "resend/svix.hpp"

#include "core/errors.hpp"

namespace azm::resend {

SvixResult verify_svix(std::string_view, std::string_view, std::string_view, std::string_view,
                       std::string_view, int64_t, int) {
  throw NotImplemented("resend::verify_svix");
}

std::string sign_svix(std::string_view, std::string_view, std::string_view, std::string_view) {
  throw NotImplemented("resend::sign_svix");
}

std::string_view to_string(SvixResult r) {
  switch (r) {
    case SvixResult::Ok: return "ok";
    case SvixResult::MissingHeaders: return "missing_headers";
    case SvixResult::BadTimestamp: return "bad_timestamp";
    case SvixResult::BadSignature: return "bad_signature";
  }
  return "bad_signature";
}

}  // namespace azm::resend
