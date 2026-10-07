// Owner: WP-A
//
// Command-line entry point used by main.cpp:
//   azmail [--env-file FILE] <command> [flags]
//   serve                          run the server (app::App::run)
//   migrate                        apply pending migrations, print the version
//   create-user --email E [--name N] [--admin] [--create-domain]
//                                  password read from stdin (first line); --create-domain calls
//                                  repo::add_domain first when the email's domain is missing
//                                  (else 422 unknown_domain); undo_send_seconds =
//                                  cfg.default_undo_send_seconds
//   reset-password --email E       password from stdin; revokes the user's sessions
//   add-domain --name D            insert into domains
//   backup --to FILE               sqlite3 online backup API (blobs are backed up separately)
//   reindex                        mail::fts_rebuild_all
//   doctor [--r2]                  config validation, DB capabilities, blob store probe,
//                                  Resend key check; --r2 also verifies presign overrides
//   blobs-migrate --to local|r2    copy → verify sha → flip blobs.storage → delete source
//   version
// Exit codes: 0 ok, 1 runtime failure, 2 usage error. Errors go to `err` without secrets.
#pragma once

#include <iosfwd>

namespace azm::app {

int run_cli(int argc, char** argv);  // uses std::cin / std::cout / std::cerr
int run_cli(int argc, char** argv, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace azm::app
