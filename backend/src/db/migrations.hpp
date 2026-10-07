// Embedded schema migrations (migration 1 = DESIGN.md §2 verbatim) and startup capability check.
#pragma once

#include "db/sqlite.hpp"

#include <span>
#include <string_view>

namespace azm::db {

struct Migration {
  int version;
  std::string_view sql;
};

// All known migrations, ascending by version.
std::span<const Migration> migrations();
int latest_version();

// Highest applied version (0 for a fresh database).
int current_version(Conn& c);

// Applies every pending migration, each inside its own BEGIN IMMEDIATE transaction, recording
// it in schema_migrations. Idempotent; safe to call on every startup. Returns the new version.
// Throws if the database is newer than this binary.
int migrate(Conn& c);

// Verifies SQLite >= 3.34 and FTS5 with the trigram tokenizer (creates and drops a TEMP
// virtual table). Throws db::Error with a clear, actionable message otherwise.
void check_capabilities(Conn& c);

}  // namespace azm::db
