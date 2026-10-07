// Owner: WP-A
// ws::Hub — registry user → live WebSocket sinks (see hub.hpp). Sinks are called OUTSIDE the
// registry lock: WsSink::send/close only post onto the session strand, but a fake sink in a test
// (or a future implementation) must never be able to deadlock the Hub.
#include "ws/hub.hpp"

#include "core/log.hpp"
#include "ws/events.hpp"
#include "ws/ws_session.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace azm::ws {

struct Hub::Impl {
  struct Entry {
    int64_t user_id = 0;
    int64_t session_id = 0;
    std::weak_ptr<WsSink> sink;
  };

  std::size_t max_per_user;
  mutable std::mutex mu;
  bool closed = false;
  uint64_t next_id = 1;
  // Ordered by registration id, so per-user iteration yields oldest first.
  std::map<uint64_t, Entry> entries;
  std::unordered_map<int64_t, std::vector<uint64_t>> by_user;  // ascending ids

  explicit Impl(std::size_t max) : max_per_user(std::max<std::size_t>(max, 1)) {}

  void erase_locked(uint64_t id) {
    auto it = entries.find(id);
    if (it == entries.end()) return;
    auto u = by_user.find(it->second.user_id);
    if (u != by_user.end()) {
      auto& v = u->second;
      v.erase(std::remove(v.begin(), v.end(), id), v.end());
      if (v.empty()) by_user.erase(u);
    }
    entries.erase(it);
  }

  // Live sinks of `user_id`; dead (expired) registrations are dropped on the way.
  std::vector<std::shared_ptr<WsSink>> sinks_of_locked(int64_t user_id) {
    std::vector<std::shared_ptr<WsSink>> out;
    auto u = by_user.find(user_id);
    if (u == by_user.end()) return out;
    std::vector<uint64_t> dead;
    for (uint64_t id : u->second) {
      if (auto s = entries[id].sink.lock()) out.push_back(std::move(s));
      else dead.push_back(id);
    }
    for (uint64_t id : dead) erase_locked(id);
    return out;
  }
};

Hub::Hub(std::size_t max_sessions_per_user) : impl_(std::make_unique<Impl>(max_sessions_per_user)) {}
Hub::~Hub() = default;

void Hub::publish(int64_t user_id, std::string type, boost::json::object data) {
  std::vector<std::shared_ptr<WsSink>> sinks;
  {
    std::lock_guard lk(impl_->mu);
    sinks = impl_->sinks_of_locked(user_id);
  }
  if (sinks.empty()) return;
  // Serialized once and shared by every socket of the user.
  auto frame = std::make_shared<const std::string>(make_frame(type, data));
  for (auto& s : sinks) s->send(frame);
}

namespace {
void revoke_all(std::vector<std::shared_ptr<WsSink>>& sinks) {
  if (sinks.empty()) return;
  auto frame = std::make_shared<const std::string>(make_frame(events::kSessionRevoked, {}));
  for (auto& s : sinks) {
    s->send(frame);
    s->close(kCloseAuthFailed, "session revoked");
  }
}
}  // namespace

void Hub::revoke_session(int64_t session_id) {
  std::vector<std::shared_ptr<WsSink>> sinks;
  {
    std::lock_guard lk(impl_->mu);
    std::vector<uint64_t> ids;
    for (const auto& [id, e] : impl_->entries)
      if (e.session_id == session_id) ids.push_back(id);
    for (uint64_t id : ids) {
      if (auto s = impl_->entries[id].sink.lock()) sinks.push_back(std::move(s));
      impl_->erase_locked(id);
    }
  }
  revoke_all(sinks);
}

void Hub::revoke_user(int64_t user_id) {
  std::vector<std::shared_ptr<WsSink>> sinks;
  {
    std::lock_guard lk(impl_->mu);
    sinks = impl_->sinks_of_locked(user_id);
    auto u = impl_->by_user.find(user_id);
    if (u != impl_->by_user.end()) {
      const auto ids = u->second;
      for (uint64_t id : ids) impl_->erase_locked(id);
    }
  }
  revoke_all(sinks);
}

uint64_t Hub::attach(int64_t user_id, int64_t session_id, std::shared_ptr<WsSink> sink) {
  if (!sink) return 0;
  std::shared_ptr<WsSink> evicted;
  uint64_t id = 0;
  {
    std::lock_guard lk(impl_->mu);
    if (!impl_->closed) {
      auto live = impl_->sinks_of_locked(user_id);  // also drops dead registrations
      if (live.size() >= impl_->max_per_user) {
        const uint64_t oldest = impl_->by_user[user_id].front();
        evicted = impl_->entries[oldest].sink.lock();
        impl_->erase_locked(oldest);
      }
      id = impl_->next_id++;
      impl_->entries.emplace(id, Impl::Entry{user_id, session_id, sink});
      impl_->by_user[user_id].push_back(id);
    }
  }
  if (id == 0) {
    // Refused during shutdown: the caller's socket is closed like every other one.
    sink->close(kCloseGoingAway, "server shutting down");
    return 0;
  }
  if (evicted) {
    log::info("ws: too many sessions, closing the oldest", {{"user_id", user_id}});
    evicted->close(kClosePolicy, "too many sessions");
  }
  return id;
}

void Hub::detach(uint64_t registration_id) {
  std::lock_guard lk(impl_->mu);
  impl_->erase_locked(registration_id);
}

void Hub::close_all() {
  std::vector<std::shared_ptr<WsSink>> sinks;
  {
    std::lock_guard lk(impl_->mu);
    impl_->closed = true;
    for (auto& [id, e] : impl_->entries)
      if (auto s = e.sink.lock()) sinks.push_back(std::move(s));
    impl_->entries.clear();
    impl_->by_user.clear();
  }
  for (auto& s : sinks) s->close(kCloseGoingAway, "server shutting down");
}

std::size_t Hub::connection_count() const {
  std::lock_guard lk(impl_->mu);
  std::size_t n = 0;
  for (const auto& [id, e] : impl_->entries) n += e.sink.expired() ? 0 : 1;
  return n;
}

std::size_t Hub::connection_count(int64_t user_id) const {
  std::lock_guard lk(impl_->mu);
  auto u = impl_->by_user.find(user_id);
  if (u == impl_->by_user.end()) return 0;
  std::size_t n = 0;
  for (uint64_t id : u->second) {
    auto it = impl_->entries.find(id);
    if (it != impl_->entries.end() && !it->second.sink.expired()) ++n;
  }
  return n;
}

}  // namespace azm::ws
