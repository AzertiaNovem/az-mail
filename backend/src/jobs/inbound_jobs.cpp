// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "jobs/handlers.hpp"

#include "core/errors.hpp"
#include "services.hpp"

namespace azm::jobs {

void register_inbound_jobs(Runner&) { throw NotImplemented("jobs::register_inbound_jobs"); }

void run_inbound_fetch(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_inbound_fetch");
}
void run_poll_receiving(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_poll_receiving");
}

mail::InboundEmail build_inbound_email(const resend::ReceivedEmail&, const mail::eml::ParsedHeaders&,
                                       std::optional<BlobRef>, std::vector<mail::InboundAttachment>,
                                       mail::InboundSource) {
  throw NotImplemented("jobs::build_inbound_email");
}

}  // namespace azm::jobs
