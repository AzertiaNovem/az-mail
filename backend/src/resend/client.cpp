// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements.
#include "resend/client.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "net/http_client.hpp"

namespace azm::resend {

struct Client::Impl {};

Client::Client(const Config&, net::HttpClient&, RateLimiter&) : impl_(std::make_unique<Impl>()) {}
Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

std::string Client::send(const SendRequest&) { throw NotImplemented("resend::Client::send"); }
SentEmail Client::get(std::string_view, Priority) { throw NotImplemented("resend::Client::get"); }
void Client::update_schedule(std::string_view, std::string_view) {
  throw NotImplemented("resend::Client::update_schedule");
}
void Client::cancel(std::string_view) { throw NotImplemented("resend::Client::cancel"); }
ReceivedEmail Client::get_received(std::string_view) {
  throw NotImplemented("resend::Client::get_received");
}
ReceivedPage Client::list_received(int, std::optional<std::string>, std::optional<std::string>) {
  throw NotImplemented("resend::Client::list_received");
}
std::vector<RecvAttachment> Client::list_received_attachments(std::string_view) {
  throw NotImplemented("resend::Client::list_received_attachments");
}
int64_t Client::download(std::string_view, const std::filesystem::path&, std::size_t) {
  throw NotImplemented("resend::Client::download");
}
DownloadResult Client::download_to(std::string_view, const std::filesystem::path&, std::size_t) {
  throw NotImplemented("resend::Client::download_to");
}
std::vector<DomainInfo> Client::list_domains() {
  throw NotImplemented("resend::Client::list_domains");
}
DomainInfo Client::get_domain(std::string_view) {
  throw NotImplemented("resend::Client::get_domain");
}

}  // namespace azm::resend
