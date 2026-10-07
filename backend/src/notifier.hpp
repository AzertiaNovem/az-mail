// Notifier: real-time invalidation hints to a user's WebSocket sessions (implemented by ws::Hub).
// Domain code never calls publish() directly; it uses db::Tx::emit(), flushed after COMMIT.
#pragma once

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace azm {

struct Notifier {
  virtual ~Notifier() = default;
  virtual void publish(int64_t user_id, std::string type, boost::json::object data) = 0;
  virtual void revoke_session(int64_t session_id) = 0;
  virtual void revoke_user(int64_t user_id) = 0;
};

// Discards everything (CLI tools, tests that don't care).
struct NullNotifier final : Notifier {
  void publish(int64_t, std::string, boost::json::object) override {}
  void revoke_session(int64_t) override {}
  void revoke_user(int64_t) override {}
};

// Records calls for assertions in tests. Thread-safe.
class RecordingNotifier final : public Notifier {
 public:
  struct Event {
    int64_t user_id = 0;
    std::string type;
    boost::json::object data;
  };

  void publish(int64_t user_id, std::string type, boost::json::object data) override {
    std::lock_guard lk(mu_);
    events_.push_back({user_id, std::move(type), std::move(data)});
  }
  void revoke_session(int64_t session_id) override {
    std::lock_guard lk(mu_);
    revoked_sessions_.push_back(session_id);
  }
  void revoke_user(int64_t user_id) override {
    std::lock_guard lk(mu_);
    revoked_users_.push_back(user_id);
  }

  std::vector<Event> events() const {
    std::lock_guard lk(mu_);
    return events_;
  }
  std::vector<Event> events_of(std::string_view type) const {
    std::lock_guard lk(mu_);
    std::vector<Event> out;
    for (const auto& e : events_)
      if (e.type == type) out.push_back(e);
    return out;
  }
  std::size_t size() const {
    std::lock_guard lk(mu_);
    return events_.size();
  }
  std::vector<int64_t> revoked_sessions() const {
    std::lock_guard lk(mu_);
    return revoked_sessions_;
  }
  std::vector<int64_t> revoked_users() const {
    std::lock_guard lk(mu_);
    return revoked_users_;
  }
  void clear() {
    std::lock_guard lk(mu_);
    events_.clear();
    revoked_sessions_.clear();
    revoked_users_.clear();
  }

 private:
  mutable std::mutex mu_;
  std::vector<Event> events_;
  std::vector<int64_t> revoked_sessions_;
  std::vector<int64_t> revoked_users_;
};

}  // namespace azm
