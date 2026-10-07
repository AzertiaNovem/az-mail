// Owner: WP-A
// WP0 stub for the config loader declared in src/config.hpp: compiles and links; WP-A
// implements env / --env-file / flag loading and validation.
#include "config.hpp"

#include "core/errors.hpp"

namespace azm {

Config load_config(const std::map<std::string, std::string>&) {
  throw NotImplemented("azm::load_config");
}

std::vector<std::string> validate_config(const Config&) {
  throw NotImplemented("azm::validate_config");
}

}  // namespace azm
