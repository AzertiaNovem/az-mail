// Owner: WP-A
// WP0 stub: compiles and links; WP-A implements the commands.
#include "app/cli.hpp"

#include "core/errors.hpp"

#include <iostream>

namespace azm::app {

int run_cli(int argc, char** argv) { return run_cli(argc, argv, std::cin, std::cout, std::cerr); }

int run_cli(int, char**, std::istream&, std::ostream&, std::ostream&) {
  throw NotImplemented("app::run_cli");
}

}  // namespace azm::app
