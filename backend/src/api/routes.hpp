// Owner: WP-D
//
// The REST route table (docs/API.md; mapping to handlers and domain calls in
// docs/CONTRACTS.md §B). route_table() is the single source of truth for method, pattern,
// auth, body mode, body limit and blocking pool of every endpoint; WP-A's session relies on it
// for 404/405/413 and pool selection. GET /api/ws is not a route (the session upgrades it).
#pragma once

#include "http/router.hpp"
#include "http/types.hpp"

#include <cstddef>
#include <vector>

namespace azm {
struct Config;
}

namespace azm::api {

// Body limits per route class (DESIGN §4 Conventions).
struct RouteLimits {
  std::size_t json = 1u << 20;      // cfg.json_body_limit (1 MiB)
  std::size_t draft = 8u << 20;     // cfg.draft_body_limit (8 MiB): drafts create/update/send
  std::size_t upload = 25u << 20;   // cfg.upload_body_limit (25 MiB): POST /api/attachments
  std::size_t webhook = 1u << 20;   // cfg.webhook_body_limit (1 MiB)
  std::size_t bodyless = 4u << 10;  // POST/DELETE without a meaningful body (body discarded)
};
RouteLimits route_limits_from(const Config& cfg);

// Every route, in registration order (56 entries; see docs/CONTRACTS.md §B).
std::vector<http::Route> route_table(const RouteLimits& limits = {});

// DESIGN §3: registers route_table(RouteLimits{}) on `router`.
void register_routes(http::Router& router);
// Same with limits from Config (what app::App uses).
void register_routes(http::Router& router, const Config& cfg);

}  // namespace azm::api
