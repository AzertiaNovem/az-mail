// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/serde.hpp"

#include "core/errors.hpp"

namespace azm::mail {

boost::json::object to_json(const Address&) { throw NotImplemented("mail::to_json(Address)"); }
boost::json::array to_json(std::span<const Address>) {
  throw NotImplemented("mail::to_json(Address list)");
}
boost::json::object to_json(const AttachmentView&) {
  throw NotImplemented("mail::to_json(AttachmentView)");
}
boost::json::object to_json(const OutboundView&) { throw NotImplemented("mail::to_json(OutboundView)"); }
boost::json::object to_json(const AuthResults&) { throw NotImplemented("mail::to_json(AuthResults)"); }
boost::json::object to_json(const MessageView&) { throw NotImplemented("mail::to_json(MessageView)"); }
boost::json::object to_json(const Participant&) { throw NotImplemented("mail::to_json(Participant)"); }
boost::json::object to_json(const ThreadListItem&) {
  throw NotImplemented("mail::to_json(ThreadListItem)");
}
boost::json::object to_json(const ThreadPage&) { throw NotImplemented("mail::to_json(ThreadPage)"); }
boost::json::object to_json(const ThreadDetail&) { throw NotImplemented("mail::to_json(ThreadDetail)"); }
boost::json::object to_json(const Draft&) { throw NotImplemented("mail::to_json(Draft)"); }
boost::json::object to_json(const SendResult&) { throw NotImplemented("mail::to_json(SendResult)"); }
boost::json::object to_json(const Counts&) { throw NotImplemented("mail::to_json(Counts)"); }
boost::json::object to_json(const ContactView&) { throw NotImplemented("mail::to_json(ContactView)"); }
boost::json::object to_json(const DeliveryEventView&) {
  throw NotImplemented("mail::to_json(DeliveryEventView)");
}

boost::json::object contacts_response(std::span<const ContactView>) {
  throw NotImplemented("mail::contacts_response");
}
boost::json::object events_response(std::span<const DeliveryEventView>) {
  throw NotImplemented("mail::events_response");
}
boost::json::object draft_response(const Draft&) { throw NotImplemented("mail::draft_response"); }
boost::json::object thread_ids_response(std::span<const int64_t>) {
  throw NotImplemented("mail::thread_ids_response");
}

Address parse_address(const boost::json::value&, std::string_view) {
  throw NotImplemented("mail::parse_address");
}
std::vector<Address> parse_address_list(const boost::json::value&, std::string_view) {
  throw NotImplemented("mail::parse_address_list");
}
DraftInput parse_draft_input(const boost::json::object&, std::string_view) {
  throw NotImplemented("mail::parse_draft_input");
}
DraftUpdate parse_draft_update(const boost::json::object&) {
  throw NotImplemented("mail::parse_draft_update");
}
SendOptions parse_send_options(const boost::json::object&) {
  throw NotImplemented("mail::parse_send_options");
}
MessagePatch parse_message_patch(const boost::json::object&) {
  throw NotImplemented("mail::parse_message_patch");
}
ThreadActionRequest parse_thread_action_request(const boost::json::object&) {
  throw NotImplemented("mail::parse_thread_action_request");
}
int64_t parse_reschedule(const boost::json::object&) { throw NotImplemented("mail::parse_reschedule"); }

}  // namespace azm::mail
