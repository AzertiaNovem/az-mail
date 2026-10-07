// Owner: WP-B
//
// JSON <-> mail structs. Output field names, nullability and enum spellings match docs/API.md
// and frontend/src/api/types.ts EXACTLY (optional → null, never omitted; times ms; ids numbers).
// Parsers throw ApiError(400, "invalid_json") / ApiError(400, "invalid_field", {field}) using
// the core/json getters; `field` names use dotted paths for nested values ("draft.to").
#pragma once

#include "mail/types.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace azm::mail {

// ---- output ------------------------------------------------------------------------------------
boost::json::object to_json(const Address& a);                      // {name, email}
boost::json::array to_json(std::span<const Address> list);          // [{name, email}, …]
boost::json::object to_json(const AttachmentView& a);               // Attachment ("inline" key)
boost::json::object to_json(const OutboundView& o);                 // Message.outbound
boost::json::object to_json(const AuthResults& a);                  // {spf, dkim, dmarc}
boost::json::object to_json(const MessageView& m);                  // Message
boost::json::object to_json(const Participant& p);                  // {name, email, is_me, unread}
boost::json::object to_json(const ThreadListItem& t);               // ThreadListItem
boost::json::object to_json(const ThreadPage& p);                   // ThreadListResponse {items, next_cursor, total}
boost::json::object to_json(const ThreadDetail& t);                 // ThreadDetail
boost::json::object to_json(const Draft& d);                        // Draft (mode always a string)
boost::json::object to_json(const SendResult& r);                   // SendResult
boost::json::object to_json(const Counts& c);                       // {inbox_unread, drafts, scheduled, spam_unread, labels:{"<id>":{unread,total}}}
boost::json::object to_json(const ContactView& c);                  // {name, email, kind}
boost::json::object to_json(const DeliveryEventView& e);            // {type, occurred_at, detail}

// Response envelopes.
boost::json::object contacts_response(std::span<const ContactView> items);       // {items:[…]}
boost::json::object events_response(std::span<const DeliveryEventView> events);  // {events:[…]}
boost::json::object draft_response(const Draft& d);                              // {draft: Draft}
boost::json::object thread_ids_response(std::span<const int64_t> ids);           // {thread_ids:[…]}

// ---- input -------------------------------------------------------------------------------------

// {name?, email} (name defaults to ""); email must satisfy is_valid_email.
Address parse_address(const boost::json::value& v, std::string_view field);
// Array of addresses (null/absent handled by callers).
std::vector<Address> parse_address_list(const boost::json::value& v, std::string_view field);

// DraftInput (absent vs null preserved per types.hpp). `prefix` prefixes field names in errors
// ("draft." inside a send body).
DraftInput parse_draft_input(const boost::json::object& o, std::string_view prefix = "");

// PUT /api/drafts/:id: DraftInput & {version (required), force?}.
struct DraftUpdate {
  int64_t version = 0;
  bool force = false;
  DraftInput input;
};
DraftUpdate parse_draft_update(const boost::json::object& o);

// POST /api/drafts/:id/send: {version (required), draft?, scheduled_at?: number|null}.
SendOptions parse_send_options(const boost::json::object& o);

// PATCH /api/messages/:id.
MessagePatch parse_message_patch(const boost::json::object& o);

// POST /api/threads/actions: {thread_ids (required, ≤ 1000), action (required), label_id?}.
// Unknown action → invalid_field "action"; label actions without label_id → invalid_field
// "label_id".
struct ThreadActionRequest {
  std::vector<int64_t> thread_ids;
  ThreadAction action = ThreadAction::Archive;
  std::optional<int64_t> label_id;
};
ThreadActionRequest parse_thread_action_request(const boost::json::object& o);

// POST /api/messages/:id/reschedule: {scheduled_at (required ms)}.
int64_t parse_reschedule(const boost::json::object& o);

}  // namespace azm::mail
