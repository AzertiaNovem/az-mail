// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/html_text.hpp"

#include "core/errors.hpp"

namespace azm::mail {

std::string html_to_text(std::string_view) { throw NotImplemented("mail::html_to_text"); }
std::string make_snippet(std::string_view, bool, std::size_t) {
  throw NotImplemented("mail::make_snippet");
}
std::string strip_quoted_text(std::string_view) { throw NotImplemented("mail::strip_quoted_text"); }

}  // namespace azm::mail
