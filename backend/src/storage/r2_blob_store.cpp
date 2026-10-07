// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "storage/r2_blob_store.hpp"

#include "core/errors.hpp"
#include "net/http_client.hpp"

namespace azm::storage {

R2Options r2_options_from(const Config&) { throw NotImplemented("storage::r2_options_from"); }

std::unique_ptr<azm::BlobStore> make_r2_blob_store(const azm::Config&, azm::net::HttpClient&) {
  throw NotImplemented("storage::make_r2_blob_store");
}

std::unique_ptr<azm::BlobStore> make_r2_blob_store(R2Options, azm::net::HttpClient&, const Clock&) {
  throw NotImplemented("storage::make_r2_blob_store");
}

std::string r2_object_key(std::string_view, std::string_view) {
  throw NotImplemented("storage::r2_object_key");
}

R2ProbeReport probe_r2(const Config&, net::HttpClient&, bool, bool) {
  throw NotImplemented("storage::probe_r2");
}

}  // namespace azm::storage
