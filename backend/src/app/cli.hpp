// Owner: WP-A
//
// Command-line entry point used by main.cpp:
//   azmail [global options] <command> [flags]       (global options may appear anywhere)
//   serve                          run the server (app::App::run)
//   migrate                        apply pending migrations, print the version
//   create-user --email E [--name N] [--admin] [--create-domain] [--password-file F]
//                                  password read from stdin (first line, no echo on a TTY) or
//                                  F; --create-domain calls repo::add_domain first when the
//                                  email's domain is missing (else 422 unknown_domain);
//                                  undo_send_seconds = cfg.default_undo_send_seconds
//   reset-password --email E [--password-file F]
//                                  password from stdin; revokes all the user's sessions
//   add-domain --name D            insert into domains
//   backup --out FILE [--force]    sqlite3 online backup API (`--to` is an alias); the copy is
//                                  quick_check-ed and renamed into place (blobs: separately)
//   reindex [--batch N]            mail::fts_rebuild_all
//   doctor [--offline] [--r2]      config validation, DB capabilities + schema, local storage,
//                                  R2 probe (write test), Resend reachability via list_domains;
//                                  --r2 also verifies presign overrides; --offline: no network
//   blobs-migrate --to local|r2 [--dry-run]
//                                  copy → verify sha → flip blobs.storage → delete source
//   version (or --version)
// Global options: --env-file FILE, --set KEY=VALUE (repeatable), --data-dir, --db-path,
// --listen, --port, --log-level, -h/--help. Precedence: defaults < env file < environment <
// flags (app/config_loader.hpp).
// Exit codes: 0 ok, 1 runtime failure, 2 usage / configuration error. Errors go to `err`
// without secrets.
#pragma once

#include <iosfwd>

namespace azm::app {

int run_cli(int argc, char** argv);  // uses std::cin / std::cout / std::cerr
int run_cli(int argc, char** argv, std::istream& in, std::ostream& out, std::ostream& err);

}  // namespace azm::app
