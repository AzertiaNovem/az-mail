// Owner: WP-B
// WP0 stub: compiles and links; WP-B implements.
#include "mail/outbound.hpp"

#include "core/errors.hpp"

namespace azm::mail {

std::optional<OutboundRow> get_outbound(db::Conn&, int64_t) { throw NotImplemented("mail::get_outbound"); }
OutboundSendPlan load_send_plan(db::Conn&, int64_t) { throw NotImplemented("mail::load_send_plan"); }
bool mark_sending(db::Tx&, int64_t) { throw NotImplemented("mail::mark_sending"); }
void mark_accepted(db::Tx&, int64_t, std::string_view, bool) {
  throw NotImplemented("mail::mark_accepted");
}
void mark_failed(db::Tx&, int64_t, std::string_view, std::string_view) {
  throw NotImplemented("mail::mark_failed");
}
void note_send_retry(db::Tx&, int64_t, std::string_view, std::string_view) {
  throw NotImplemented("mail::note_send_retry");
}
int64_t switch_to_local_schedule(db::Tx&, int64_t) {
  throw NotImplemented("mail::switch_to_local_schedule");
}
EventApplyResult apply_outbound_event(db::Tx&, const OutboundEvent&) {
  throw NotImplemented("mail::apply_outbound_event");
}
void set_outbound_message_id(db::Tx&, int64_t, std::string_view) {
  throw NotImplemented("mail::set_outbound_message_id");
}
std::optional<int64_t> find_outbound(db::Conn&, std::optional<std::string_view>,
                                     std::optional<std::string_view>) {
  throw NotImplemented("mail::find_outbound");
}
std::vector<ReconcileItem> outbound_to_reconcile(db::Conn&, int64_t, int64_t, int) {
  throw NotImplemented("mail::outbound_to_reconcile");
}
CancelPlan begin_cancel_schedule(db::Tx&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::begin_cancel_schedule");
}
Draft finish_cancel_schedule(db::Tx&, const SignedUrls&, int64_t, int64_t) {
  throw NotImplemented("mail::finish_cancel_schedule");
}
ReschedulePlan begin_reschedule(db::Tx&, const Config&, int64_t, int64_t, int64_t, int64_t) {
  throw NotImplemented("mail::begin_reschedule");
}
void finish_reschedule(db::Tx&, int64_t, int64_t, int64_t) {
  throw NotImplemented("mail::finish_reschedule");
}
SendResult retry_failed_send(db::Tx&, int64_t, int64_t, int64_t) {
  throw NotImplemented("mail::retry_failed_send");
}
int64_t admin_retry_outbound(db::Tx&, int64_t, int64_t) {
  throw NotImplemented("mail::admin_retry_outbound");
}

}  // namespace azm::mail
