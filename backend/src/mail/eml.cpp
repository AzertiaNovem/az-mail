// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements (iconv is linked into azmail_lib).
#include "mail/eml.hpp"

#include "core/errors.hpp"

namespace azm::mail::eml {

std::optional<std::string> HeaderBlock::get(std::string_view) const {
  throw NotImplemented("eml::HeaderBlock::get");
}
std::vector<std::string> HeaderBlock::get_all(std::string_view) const {
  throw NotImplemented("eml::HeaderBlock::get_all");
}
HeaderBlock parse_header_block(std::string_view, std::size_t) {
  throw NotImplemented("eml::parse_header_block");
}
std::string read_file_prefix(const std::filesystem::path&, std::size_t) {
  throw NotImplemented("eml::read_file_prefix");
}
std::string unfold(std::string_view) { throw NotImplemented("eml::unfold"); }
std::string decode_rfc2047(std::string_view) { throw NotImplemented("eml::decode_rfc2047"); }
std::optional<std::string> to_utf8(std::string_view, std::string_view) {
  throw NotImplemented("eml::to_utf8");
}
std::vector<std::string> parse_msgid_list(std::string_view) {
  throw NotImplemented("eml::parse_msgid_list");
}
std::optional<std::string> parse_msgid(std::string_view) { throw NotImplemented("eml::parse_msgid"); }
ParsedHeaders extract_headers(const HeaderBlock&) { throw NotImplemented("eml::extract_headers"); }
ParsedHeaders parse_headers(std::string_view) { throw NotImplemented("eml::parse_headers"); }

}  // namespace azm::mail::eml
