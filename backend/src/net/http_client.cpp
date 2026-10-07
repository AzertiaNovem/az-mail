// Owner: WP-C
// WP0 stub: compiles and links; WP-C implements the transport.
#include "net/http_client.hpp"

#include "config.hpp"
#include "core/errors.hpp"
#include "core/strings.hpp"

namespace azm::net {

struct HttpClient::Impl {
  ClientOptions opts;
};

ClientOptions client_options_from(const Config&) {
  throw NotImplemented("net::client_options_from");
}

std::optional<std::string> HttpResponse::header(std::string_view name) const {
  for (const auto& [k, v] : headers)
    if (iequals(k, name)) return v;
  return std::nullopt;
}

HttpClient::HttpClient(ClientOptions opts) : impl_(std::make_unique<Impl>(Impl{std::move(opts)})) {}
HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpResponse HttpClient::send(const HttpRequest&) { throw NotImplemented("net::HttpClient::send"); }

const ClientOptions& HttpClient::options() const { return impl_->opts; }

}  // namespace azm::net
