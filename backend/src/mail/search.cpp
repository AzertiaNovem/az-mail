// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/search.hpp"

#include "core/errors.hpp"

namespace azm::mail {

ParsedQuery parse_search(std::string_view) { throw NotImplemented("mail::parse_search"); }
SqlFilter compile_search(std::string_view, int, int64_t) {
  throw NotImplemented("mail::compile_search");
}
std::optional<int64_t> parse_size(std::string_view) { throw NotImplemented("mail::parse_size"); }
std::optional<int64_t> parse_relative_age_ms(std::string_view) {
  throw NotImplemented("mail::parse_relative_age_ms");
}
std::optional<int64_t> parse_search_date(std::string_view, int) {
  throw NotImplemented("mail::parse_search_date");
}

}  // namespace azm::mail
