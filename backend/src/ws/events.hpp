// Owner: WP-A (names and payload shapes frozen by WP0; additive changes only)
//
// WebSocket event type names and payload builders (docs/API.md "WebSocket protocol").
//
//  * Frames are FLAT JSON objects: ws::Hub serializes `{"type": <type>, ...data}` (never a nested
//    "data" key). Events without contents are `{"type": <type>}`.
//  * Domain code publishes ONLY through db::Tx::emit(user_id, type, data) (flushed after COMMIT),
//    using the builders below so payloads match the frontend's WsServerEvent types exactly.
//  * Events are invalidation hints, not data: dropping one is harmless.
#pragma once

#include "core/address.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace azm::ws {

// ---- server → client event types -----------------------------------------------------------
namespace events {
inline constexpr std::string_view kReady = "ready";                     // {user_id, server_time}
inline constexpr std::string_view kPong = "pong";                       // –
inline constexpr std::string_view kMailNew = "mail.new";                // see mail_new_payload
inline constexpr std::string_view kThreadsChanged = "threads.changed";  // {thread_ids}
inline constexpr std::string_view kOutboundStatus = "outbound.status";  // see outbound_status_payload
inline constexpr std::string_view kLabelsChanged = "labels.changed";    // –
inline constexpr std::string_view kSettingsChanged = "settings.changed";  // –
inline constexpr std::string_view kSessionRevoked = "session.revoked";    // –
}  // namespace events

// ---- client → server message types ---------------------------------------------------------
namespace client_messages {
inline constexpr std::string_view kAuth = "auth";  // {"type":"auth","token":"…"} within 5 s
inline constexpr std::string_view kPing = "ping";  // {"type":"ping"}
}  // namespace client_messages

// Close code sent when first-message auth fails or times out, and when the session is revoked.
inline constexpr std::uint16_t kCloseAuthFailed = 4401;

// ---- payload builders (the `data` argument of Tx::emit / Notifier::publish) ----------------

// ready: {user_id, server_time(ms)}
inline boost::json::object ready_payload(int64_t user_id, int64_t server_time_ms) {
  boost::json::object o;
  o["user_id"] = user_id;
  o["server_time"] = server_time_ms;
  return o;
}

// mail.new: {thread_id, message_id, from:{name,email}, subject, snippet, in_inbox, is_spam}
inline boost::json::object mail_new_payload(int64_t thread_id, int64_t message_id,
                                            const Address& from, std::string_view subject,
                                            std::string_view snippet, bool in_inbox,
                                            bool is_spam) {
  boost::json::object addr;
  addr["name"] = from.name;
  addr["email"] = from.email;
  boost::json::object o;
  o["thread_id"] = thread_id;
  o["message_id"] = message_id;
  o["from"] = std::move(addr);
  o["subject"] = subject;
  o["snippet"] = snippet;
  o["in_inbox"] = in_inbox;
  o["is_spam"] = is_spam;
  return o;
}

// threads.changed: {thread_ids:[…]}
inline boost::json::object threads_changed_payload(std::span<const int64_t> thread_ids) {
  boost::json::array ids;
  ids.reserve(thread_ids.size());
  for (int64_t id : thread_ids) ids.emplace_back(id);
  boost::json::object o;
  o["thread_ids"] = std::move(ids);
  return o;
}

// outbound.status: {message_id, thread_id, outbound_id, status, status_detail:string|null}
// `status` is the wire spelling (mail::to_string(OutboundStatus)).
inline boost::json::object outbound_status_payload(int64_t message_id, int64_t thread_id,
                                                   int64_t outbound_id, std::string_view status,
                                                   std::optional<std::string_view> status_detail) {
  boost::json::object o;
  o["message_id"] = message_id;
  o["thread_id"] = thread_id;
  o["outbound_id"] = outbound_id;
  o["status"] = status;
  if (status_detail) o["status_detail"] = *status_detail;
  else o["status_detail"] = nullptr;
  return o;
}

// The serialized frame `{"type":type, ...data}` (what ws::Hub writes to each socket). A "type"
// key inside `data` is ignored (the explicit type wins).
inline std::string make_frame(std::string_view type, const boost::json::object& data) {
  boost::json::object frame;
  frame.reserve(data.size() + 1);
  frame["type"] = type;
  for (const auto& kv : data)
    if (kv.key() != "type") frame[kv.key()] = kv.value();
  return boost::json::serialize(frame);
}

}  // namespace azm::ws
