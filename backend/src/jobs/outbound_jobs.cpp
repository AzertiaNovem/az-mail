// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements. register_all_jobs lives here.
#include "jobs/handlers.hpp"

#include "core/errors.hpp"
#include "services.hpp"

namespace azm::jobs {

void register_outbound_jobs(Runner&) { throw NotImplemented("jobs::register_outbound_jobs"); }

void register_all_jobs(Runner& runner) {
  register_outbound_jobs(runner);
  register_inbound_jobs(runner);
  register_maintenance_jobs(runner);
}

void run_outbound_send(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_outbound_send");
}
void run_outbound_fetch_meta(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_outbound_fetch_meta");
}
void run_outbound_reconcile(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_outbound_reconcile");
}
std::chrono::milliseconds outbound_backoff(int) { throw NotImplemented("jobs::outbound_backoff"); }

}  // namespace azm::jobs
