// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/inbound.hpp"

#include "core/errors.hpp"

namespace azm::mail {

std::vector<LocalRecipient> resolve_local_recipients(db::Conn&, std::span<const std::string>) {
  throw NotImplemented("mail::resolve_local_recipients");
}
std::vector<std::string> unknown_local_recipients(db::Conn&, std::span<const std::string>) {
  throw NotImplemented("mail::unknown_local_recipients");
}
DeliveryResult deliver_inbound(db::Tx&, const InboundEmail&, const DeliveryOptions&) {
  throw NotImplemented("mail::deliver_inbound");
}
std::vector<std::string> spam_warnings(const AuthResults&, bool, bool) {
  throw NotImplemented("mail::spam_warnings");
}
int64_t record_inbound_pending(db::Tx&, std::string_view, InboundSource, int64_t) {
  throw NotImplemented("mail::record_inbound_pending");
}
void mark_inbound_failed(db::Tx&, std::string_view, std::string_view, int64_t) {
  throw NotImplemented("mail::mark_inbound_failed");
}
std::optional<InboundState> inbound_state(db::Conn&, std::string_view) {
  throw NotImplemented("mail::inbound_state");
}

}  // namespace azm::mail
