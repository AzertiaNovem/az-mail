#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

using namespace azm;

namespace {

// Captures log output for the duration of a test and restores level + sink afterwards.
struct Capture {
  std::vector<std::string> lines;
  log::Level saved = log::level();
  Capture() {
    log::set_sink([this](std::string_view l) { lines.emplace_back(l); });
  }
  ~Capture() {
    log::set_sink({});
    log::set_level(saved);
  }
};

}  // namespace

TEST_CASE("parse_level and level_name", "[log]") {
  CHECK(log::parse_level("trace") == log::Level::Trace);
  CHECK(log::parse_level("DEBUG") == log::Level::Debug);
  CHECK(log::parse_level("Info") == log::Level::Info);
  CHECK(log::parse_level("warning") == log::Level::Warn);
  CHECK(log::parse_level("error") == log::Level::Error);
  CHECK(log::parse_level("off") == log::Level::Off);
  CHECK_FALSE(log::parse_level("verbose"));
  CHECK(log::level_name(log::Level::Warn) == "WARN");
}

TEST_CASE("format_line layout and quoting", "[log]") {
  const std::string line = log::format_line(
      1791374400000, log::Level::Info, "req-1", "request done",
      {{"status", 200}, {"path", "/api/threads"}, {"note", "has space"}, {"q", "a\"b"},
       {"ok", true}, {"empty", ""}, {"ms", 1.5}});
  CHECK(line ==
        "2026-10-07T12:00:00.000Z INFO [req-1] request done status=200 path=/api/threads "
        "note=\"has space\" q=\"a\\\"b\" ok=true empty=\"\" ms=1.500000");
  // No request id; control characters in the message are flattened.
  CHECK(log::format_line(0, log::Level::Error, "", "line1\nline2", {}) ==
        "1970-01-01T00:00:00.000Z ERROR [-] line1 line2");
  CHECK(log::format_line(0, log::Level::Warn, "", "m", {{"v", "x\ny"}}) ==
        "1970-01-01T00:00:00.000Z WARN [-] m v=\"x\\ny\"");
}

TEST_CASE("level filtering and sink", "[log]") {
  Capture cap;
  log::set_level(log::Level::Warn);
  log::info("hidden");
  log::warn("shown", {{"k", 1}});
  log::error("also shown");
  REQUIRE(cap.lines.size() == 2);
  CHECK(cap.lines[0].find(" WARN [-] shown k=1\n") != std::string::npos);
  CHECK(cap.lines[1].find(" ERROR ") != std::string::npos);
  CHECK(log::enabled(log::Level::Error));
  CHECK_FALSE(log::enabled(log::Level::Debug));
  log::set_level(log::Level::Off);
  log::error("nothing");
  CHECK(cap.lines.size() == 2);
}

TEST_CASE("thread-local request id", "[log]") {
  Capture cap;
  log::set_level(log::Level::Info);
  CHECK(log::request_id().empty());
  {
    log::ScopedRequestId rid("abc123");
    CHECK(log::request_id() == "abc123");
    log::info("inside");
    {
      log::ScopedRequestId nested("nested");
      CHECK(log::request_id() == "nested");
    }
    CHECK(log::request_id() == "abc123");
    std::string other_thread_id = "unset";
    std::thread([&] { other_thread_id = log::request_id(); }).join();
    CHECK(other_thread_id.empty());
  }
  CHECK(log::request_id().empty());
  REQUIRE(cap.lines.size() == 1);
  CHECK(cap.lines[0].find(" INFO [abc123] inside") != std::string::npos);
}
