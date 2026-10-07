// Owner: WP-A
// azmail entry point: everything is in app::run_cli (serve | migrate | create-user |
// reset-password | add-domain | backup | reindex | doctor | blobs-migrate | version).
#include "app/cli.hpp"

int main(int argc, char** argv) { return azm::app::run_cli(argc, argv); }
