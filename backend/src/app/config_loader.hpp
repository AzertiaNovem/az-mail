// Owner: WP-A
//
// Config loading details behind azm::load_config / azm::validate_config (config.hpp):
//  * Sources, lowest to highest precedence: built-in defaults < --env-file < real environment <
//    CLI overrides (`--set KEY=VALUE`, `--port`, …). Keys are the env names documented in
//    config.hpp (AZMAIL_*, RESEND_*, R2_*). Unknown AZMAIL_/R2_/RESEND_ keys are reported.
//  * Value syntax: integers in decimal; sizes accept K/M/G (KiB/MiB/GiB) suffixes ("25M");
//    booleans 1/0/true/false/yes/no/on/off; lists are comma-separated (blanks ignored).
//  * AZMAIL_SECRET may be given raw (used as is), as "hex:<hex>" or "base64:<b64>".
//  * Derived defaults: AZMAIL_DB_PATH defaults to <AZMAIL_DATA_DIR>/azmail.db when only the
//    data dir is set; AZMAIL_PUBLIC_API_URL defaults to http://<listen>:<port> when only the
//    listen address/port are set.
#pragma once

#include "config.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm::app {

using EnvMap = std::map<std::string, std::string, std::less<>>;

// Parses an env file: KEY=VALUE per line; blank lines and '#' comments ignored; an optional
// leading "export "; values may be single-quoted (literal) or double-quoted (\n \t \" \\ escapes);
// unquoted values end at " #" (inline comment) and are trimmed. Malformed lines are reported in
// `errors` ("line N: …") and skipped.
EnvMap parse_env_file(std::string_view text, std::vector<std::string>& errors);
// Reads and parses a file; throws std::runtime_error when it cannot be read or has errors.
EnvMap read_env_file(const std::filesystem::path& path);

// The process environment restricted to AZMAIL_*, RESEND_* and R2_* variables.
EnvMap process_environment();

struct ConfigLoad {
  Config cfg;
  std::vector<std::string> problems;  // unparseable values (the field keeps its default)
  std::vector<std::string> warnings;  // unknown AZMAIL_*/RESEND_*/R2_* keys (ignored)
};
// Builds a Config from env-style keys only (no process environment). Never throws.
ConfigLoad config_from_env(const EnvMap& env);

// Merges sources by precedence (later wins): file < process env < overrides.
EnvMap merge_env(const EnvMap& file, const EnvMap& process, const EnvMap& overrides);

enum class ConfigPurpose {
  Serve,    // `azmail serve` / `doctor`: secrets and network settings required
  Offline,  // migrate, create-user, backup, …: only database / storage settings matter
};
// Semantic validation (validate_config(cfg) == validate_config(cfg, Serve)). Messages are
// bilingual ("English / 中文") and never contain secret values.
std::vector<std::string> validate_config(const Config& cfg, ConfigPurpose purpose);
// Non-fatal advice (e.g. pool sizing, missing webhook secret).
std::vector<std::string> config_warnings(const Config& cfg);

// AZMAIL_SECRET decoding ("hex:", "base64:" prefixes, else raw). nullopt for a bad encoding.
std::optional<std::string> decode_secret(std::string_view value);

// ---- additive (F2): secret strength ----------------------------------------------------------
// True for an obviously non-random secret: fewer than 8 distinct bytes, or two or more copies
// of one shorter unit. Never true for `openssl rand -hex 32` / base64 output in practice.
bool degenerate_secret(std::string_view secret);
// Why `secret` (decoded AZMAIL_SECRET) must not be used: empty → required; the placeholder of
// deploy/azmail.env.example (anything containing CHANGE_ME / change-me / placeholder …);
// shorter than 32 bytes; degenerate_secret.
// nullopt when acceptable. Bilingual, never contains the secret.
std::optional<std::string> server_secret_problem(std::string_view secret);
// Same for RESEND_WEBHOOK_SECRET (non-empty): must be "whsec_" + base64 of >= 16 bytes, not a
// placeholder (one repeated character, "xxxx…", CHANGE_ME …).
std::optional<std::string> webhook_secret_problem(std::string_view secret);

}  // namespace azm::app
