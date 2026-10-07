// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/drafts.hpp"

#include "core/errors.hpp"

namespace azm::mail {

std::optional<Draft> get_draft(db::Conn&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::get_draft");
}
Draft create_draft(db::Tx&, const SignedUrls&, int64_t, const DraftInput&) {
  throw NotImplemented("mail::create_draft");
}
Draft update_draft(db::Tx&, const SignedUrls&, int64_t, int64_t, int64_t, const DraftInput&, bool) {
  throw NotImplemented("mail::update_draft");
}
void delete_draft(db::Tx&, int64_t, int64_t) { throw NotImplemented("mail::delete_draft"); }
SendResult queue_send(db::Tx&, const Config&, const SignedUrls&, int64_t, int64_t, const SendOptions&) {
  throw NotImplemented("mail::queue_send");
}
SendResult queue_send(db::Tx&, const Config&, int64_t, int64_t, const SendOptions&) {
  throw NotImplemented("mail::queue_send");
}
Draft undo_send(db::Tx&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::undo_send");
}
SenderIdentity resolve_sender(db::Conn&, int64_t, int64_t) {
  throw NotImplemented("mail::resolve_sender");
}
int64_t default_from_address(db::Conn&, int64_t) { throw NotImplemented("mail::default_from_address"); }

}  // namespace azm::mail
