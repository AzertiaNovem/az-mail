// Owner: WP-A
//
// ws::Hub — the production Notifier (DESIGN A4): registry user → live WebSocket sinks.
//  * publish(user, type, data) serializes ONE frame (ws::make_frame: flat {type, ...data}) and
//    hands it to every sink of that user. Called from db::Pool after COMMIT (via Tx::emit),
//    from any thread; never blocks on sockets.
//  * revoke_session / revoke_user: send {"type":"session.revoked"} to the affected sinks, then
//    close them with ws::kCloseAuthFailed (4401). Used on logout, password change, disable.
//  * Sinks are held weakly-owned by their session; a sink that died is dropped lazily.
// Thread-safe.
#pragma once

#include "notifier.hpp"

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace azm::ws {

// One authenticated socket as seen by the Hub. Implemented by the WS session (ws_session.cpp)
// and by fakes in tests.
struct WsSink {
  virtual ~WsSink() = default;
  // Queue a text frame. Thread-safe; posts onto the session strand. When the session's queue
  // already holds cfg.ws_queue_cap frames the session closes itself (client resyncs).
  virtual void send(std::shared_ptr<const std::string> frame) = 0;
  // Close with `code` (e.g. 4401, 1001 going_away). Thread-safe, idempotent.
  virtual void close(std::uint16_t code, std::string_view reason) = 0;
};

class Hub final : public Notifier {
 public:
  // `max_sessions_per_user` = cfg.ws_max_sessions_per_user; attaching beyond it closes that
  // user's oldest sink with 1008 (policy violation).
  explicit Hub(std::size_t max_sessions_per_user = 32);
  ~Hub() override;
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  void publish(int64_t user_id, std::string type, boost::json::object data) override;
  void revoke_session(int64_t session_id) override;
  void revoke_user(int64_t user_id) override;

  // Registers an authenticated socket; returns a registration id for detach(). Called by the
  // WS session after successful first-message auth (before it sends "ready").
  uint64_t attach(int64_t user_id, int64_t session_id, std::shared_ptr<WsSink> sink);
  // Unregisters (idempotent). Called when the socket ends.
  void detach(uint64_t registration_id);

  // Shutdown (DESIGN A7): closes every sink with 1001 going_away and refuses new attaches.
  void close_all();

  std::size_t connection_count() const;
  std::size_t connection_count(int64_t user_id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azm::ws
