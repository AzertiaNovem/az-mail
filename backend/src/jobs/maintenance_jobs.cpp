// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "jobs/handlers.hpp"

#include "core/errors.hpp"
#include "services.hpp"

namespace azm::jobs {

void register_maintenance_jobs(Runner&) { throw NotImplemented("jobs::register_maintenance_jobs"); }

void run_purge_trash(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_purge_trash");
}
void run_gc_blobs(Services&, const Job&, std::stop_token) { throw NotImplemented("jobs::run_gc_blobs"); }
void run_gc_housekeeping(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_gc_housekeeping");
}
void run_db_optimize(Services&, const Job&, std::stop_token) {
  throw NotImplemented("jobs::run_db_optimize");
}

}  // namespace azm::jobs
