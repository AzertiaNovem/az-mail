// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "jobs/webhook_dispatch.hpp"

#include "core/errors.hpp"

namespace azm::jobs {

std::string_view to_string(WebhookResult r) {
  switch (r) {
    case WebhookResult::Applied: return "applied";
    case WebhookResult::Enqueued: return "enqueued";
    case WebhookResult::IgnoredUnknown: return "ignored_unknown";
    case WebhookResult::Duplicate: return "duplicate";
    case WebhookResult::Error: return "error";
  }
  return "error";
}

WebhookOutcome process_webhook(db::Tx&, const resend::WebhookEnvelope&, std::string_view,
                               std::string_view, int64_t) {
  throw NotImplemented("jobs::process_webhook");
}

}  // namespace azm::jobs
