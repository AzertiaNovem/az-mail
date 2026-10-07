// Owner: WP-A — ws::Hub fan-out, revocation, per-user cap, shutdown (fake sinks, no sockets).
#include "notifier.hpp"
#include "ws/events.hpp"
#include "ws/hub.hpp"
#include "ws/ws_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json/parse.hpp>

#include <mutex>
#include <thread>

using namespace azm;

namespace {

struct FakeSink final : ws::WsSink {
  mutable std::mutex mu;
  std::vector<std::string> frames;
  std::vector<std::pair<std::uint16_t, std::string>> closes;

  void send(std::shared_ptr<const std::string> frame) override {
    std::lock_guard lk(mu);
    frames.push_back(*frame);
  }
  void close(std::uint16_t code, std::string_view reason) override {
    std::lock_guard lk(mu);
    closes.emplace_back(code, std::string(reason));
  }
  std::vector<std::string> got() const {
    std::lock_guard lk(mu);
    return frames;
  }
  std::vector<std::pair<std::uint16_t, std::string>> closed() const {
    std::lock_guard lk(mu);
    return closes;
  }
};

boost::json::object parse(const std::string& s) { return boost::json::parse(s).as_object(); }

}  // namespace

TEST_CASE("hub: publish fans out flat frames to every socket of the user", "[hub]") {
  ws::Hub hub;
  auto a1 = std::make_shared<FakeSink>();
  auto a2 = std::make_shared<FakeSink>();
  auto b = std::make_shared<FakeSink>();
  const auto ra1 = hub.attach(1, 10, a1);
  const auto ra2 = hub.attach(1, 11, a2);
  const auto rb = hub.attach(2, 20, b);
  CHECK(ra1 != 0);
  CHECK(ra2 != ra1);
  CHECK(rb != 0);
  CHECK(hub.connection_count() == 3);
  CHECK(hub.connection_count(1) == 2);
  CHECK(hub.connection_count(3) == 0);

  const int64_t ids[] = {5, 6};
  Notifier& n = hub;
  n.publish(1, std::string(ws::events::kThreadsChanged), ws::threads_changed_payload(ids));
  REQUIRE(a1->got().size() == 1);
  REQUIRE(a2->got().size() == 1);
  CHECK(b->got().empty());
  auto f = parse(a1->got()[0]);
  CHECK(f["type"] == "threads.changed");
  CHECK(f["thread_ids"].as_array().size() == 2);
  CHECK_FALSE(f.contains("data"));  // flat frames (API.md Addendum B)

  n.publish(2, std::string(ws::events::kLabelsChanged), {});
  REQUIRE(b->got().size() == 1);
  CHECK(b->got()[0] == R"({"type":"labels.changed"})");

  // Publishing to a user without sockets is a no-op.
  CHECK_NOTHROW(n.publish(99, "mail.new", {}));

  hub.detach(ra1);
  hub.detach(ra1);  // idempotent
  CHECK(hub.connection_count(1) == 1);
  n.publish(1, "settings.changed", {});
  CHECK(a1->got().size() == 1);
  CHECK(a2->got().size() == 2);
}

TEST_CASE("hub: dead sinks are dropped lazily", "[hub]") {
  ws::Hub hub;
  auto keep = std::make_shared<FakeSink>();
  hub.attach(1, 1, keep);
  {
    auto gone = std::make_shared<FakeSink>();
    hub.attach(1, 2, gone);
  }
  CHECK(hub.connection_count(1) == 1);  // the expired weak reference is not counted
  hub.publish(1, "pong", {});
  CHECK(keep->got().size() == 1);
  CHECK(hub.connection_count() == 1);
}

TEST_CASE("hub: revoke_session sends session.revoked then closes 4401", "[hub]") {
  ws::Hub hub;
  auto s10 = std::make_shared<FakeSink>();
  auto s11 = std::make_shared<FakeSink>();
  auto other = std::make_shared<FakeSink>();
  hub.attach(1, 10, s10);
  hub.attach(1, 11, s11);
  hub.attach(2, 10, other);  // session ids are global; same id under another user is impossible
                             // in practice but must still be matched by session id only
  hub.revoke_session(10);
  REQUIRE(s10->got().size() == 1);
  CHECK(parse(s10->got()[0])["type"] == "session.revoked");
  REQUIRE(s10->closed().size() == 1);
  CHECK(s10->closed()[0].first == ws::kCloseAuthFailed);
  CHECK(s11->got().empty());
  CHECK(s11->closed().empty());
  CHECK(other->closed().size() == 1);
  // Revoked sockets no longer receive events.
  CHECK(hub.connection_count(1) == 1);
  hub.publish(1, "labels.changed", {});
  CHECK(s10->got().size() == 1);
  CHECK(s11->got().size() == 1);
  hub.revoke_session(12345);  // unknown: no-op
}

TEST_CASE("hub: revoke_user closes all of a user's sockets", "[hub]") {
  ws::Hub hub;
  auto a = std::make_shared<FakeSink>();
  auto b = std::make_shared<FakeSink>();
  auto c = std::make_shared<FakeSink>();
  hub.attach(7, 1, a);
  hub.attach(7, 2, b);
  hub.attach(8, 3, c);
  hub.revoke_user(7);
  for (const auto& s : {a, b}) {
    REQUIRE(s->got().size() == 1);
    CHECK(parse(s->got()[0])["type"] == "session.revoked");
    REQUIRE(s->closed().size() == 1);
    CHECK(s->closed()[0].first == 4401);
  }
  CHECK(c->closed().empty());
  CHECK(hub.connection_count(7) == 0);
  CHECK(hub.connection_count() == 1);
  hub.revoke_user(7);  // again: no-op
  CHECK(a->closed().size() == 1);
}

TEST_CASE("hub: per-user cap closes the oldest socket with 1008", "[hub]") {
  ws::Hub hub(2);
  auto s1 = std::make_shared<FakeSink>();
  auto s2 = std::make_shared<FakeSink>();
  auto s3 = std::make_shared<FakeSink>();
  hub.attach(1, 1, s1);
  hub.attach(1, 2, s2);
  hub.attach(1, 3, s3);
  REQUIRE(s1->closed().size() == 1);
  CHECK(s1->closed()[0].first == ws::kClosePolicy);
  CHECK(s2->closed().empty());
  CHECK(hub.connection_count(1) == 2);
  hub.publish(1, "pong", {});
  CHECK(s1->got().empty());
  CHECK(s2->got().size() == 1);
  CHECK(s3->got().size() == 1);
}

TEST_CASE("hub: close_all sends going_away and refuses new attaches", "[hub]") {
  ws::Hub hub;
  auto a = std::make_shared<FakeSink>();
  auto b = std::make_shared<FakeSink>();
  hub.attach(1, 1, a);
  hub.attach(2, 2, b);
  hub.close_all();
  CHECK(a->closed().at(0).first == ws::kCloseGoingAway);
  CHECK(b->closed().at(0).first == ws::kCloseGoingAway);
  CHECK(hub.connection_count() == 0);
  auto late = std::make_shared<FakeSink>();
  CHECK(hub.attach(1, 3, late) == 0);
  CHECK(late->closed().at(0).first == ws::kCloseGoingAway);
  CHECK(hub.attach(1, 4, nullptr) == 0);
}

TEST_CASE("hub: concurrent publish/attach/detach is safe", "[hub]") {
  ws::Hub hub(1000);
  auto sink = std::make_shared<FakeSink>();
  hub.attach(1, 1, sink);
  std::atomic<bool> stop{false};
  std::thread publisher([&] {
    while (!stop) hub.publish(1, "threads.changed", ws::threads_changed_payload({}));
  });
  std::vector<std::shared_ptr<FakeSink>> keep;
  for (int i = 0; i < 300; ++i) {
    auto s = std::make_shared<FakeSink>();
    keep.push_back(s);
    const auto id = hub.attach(1, 100 + i, s);
    if (i % 2 == 0) hub.detach(id);
  }
  stop = true;
  publisher.join();
  CHECK(hub.connection_count(1) == 151);
  CHECK_FALSE(sink->got().empty());
}
