// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/mailbox.hpp"

#include "core/errors.hpp"

namespace azm::mail {

ThreadPage list_threads(db::Conn&, int64_t, const ThreadQuery&) {
  throw NotImplemented("mail::list_threads");
}
std::optional<ThreadDetail> get_thread(db::Conn&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::get_thread");
}
std::optional<MessageView> get_message(db::Conn&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::get_message");
}
Counts counts(db::Conn&, int64_t) { throw NotImplemented("mail::counts"); }
std::vector<int64_t> apply_thread_action(db::Tx&, int64_t, std::span<const int64_t>, ThreadAction,
                                         std::optional<int64_t>) {
  throw NotImplemented("mail::apply_thread_action");
}
void patch_message(db::Tx&, int64_t, int64_t, const MessagePatch&) {
  throw NotImplemented("mail::patch_message");
}
std::optional<std::vector<DeliveryEventView>> message_events(db::Conn&, int64_t, int64_t) {
  throw NotImplemented("mail::message_events");
}
std::vector<int64_t> message_label_ids(db::Conn&, int64_t, int64_t) {
  throw NotImplemented("mail::message_label_ids");
}
std::vector<ContactView> search_contacts(db::Conn&, int64_t, std::string_view, int) {
  throw NotImplemented("mail::search_contacts");
}
void upsert_contact(db::Tx&, int64_t, const Address&, double, int64_t) {
  throw NotImplemented("mail::upsert_contact");
}
PurgeResult purge_trash(db::Tx&, int64_t, int, int, int) { throw NotImplemented("mail::purge_trash"); }

}  // namespace azm::mail
