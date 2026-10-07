#include "core/log.hpp"

#include "core/time.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace azm::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mu;
std::function<void(std::string_view)> g_sink;  // guarded by g_mu

thread_local std::string t_request_id;

bool needs_quotes(std::string_view v) {
  if (v.empty()) return true;
  for (unsigned char c : v) {
    if (c <= 0x20 || c == '"' || c == '=' || c == '\\' || c == 0x7f) return true;
  }
  return false;
}

void append_value(std::string& out, std::string_view v) {
  if (!needs_quotes(v)) {
    out.append(v);
    return;
  }
  out.push_back('"');
  for (unsigned char c : v) {
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20 || c == 0x7f) {
          static constexpr char kHex[] = "0123456789abcdef";
          out.append("\\x");
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// Messages are single-line: control characters become spaces.
void append_message(std::string& out, std::string_view m) {
  for (unsigned char c : m) out.push_back(c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c));
}

}  // namespace

std::optional<Level> parse_level(std::string_view s) {
  std::string l;
  l.reserve(s.size());
  for (char c : s) l.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  if (l == "trace") return Level::Trace;
  if (l == "debug") return Level::Debug;
  if (l == "info") return Level::Info;
  if (l == "warn" || l == "warning") return Level::Warn;
  if (l == "error") return Level::Error;
  if (l == "off" || l == "none") return Level::Off;
  return std::nullopt;
}

std::string_view level_name(Level l) {
  switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Off: return "OFF";
  }
  return "?";
}

void set_level(Level l) { g_level.store(static_cast<int>(l), std::memory_order_relaxed); }
Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }
bool enabled(Level l) {
  return l != Level::Off && static_cast<int>(l) >= g_level.load(std::memory_order_relaxed);
}

std::string format_line(int64_t ts_ms, Level l, std::string_view request_id, std::string_view msg,
                        std::initializer_list<Field> fields) {
  std::string line;
  line.reserve(64 + msg.size() + fields.size() * 24);
  line.append(iso8601_utc(ts_ms));
  line.push_back(' ');
  line.append(level_name(l));
  line.append(" [");
  line.append(request_id.empty() ? std::string_view("-") : request_id);
  line.append("] ");
  append_message(line, msg);
  for (const auto& f : fields) {
    line.push_back(' ');
    line.append(f.key);
    line.push_back('=');
    append_value(line, f.value);
  }
  return line;
}

void write(Level l, std::string_view msg, std::initializer_list<Field> fields) {
  if (!enabled(l)) return;
  std::string line = format_line(now_ms(), l, t_request_id, msg, fields);
  line.push_back('\n');
  std::lock_guard lk(g_mu);
  if (g_sink) {
    g_sink(line);
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
}

void set_sink(std::function<void(std::string_view)> sink) {
  std::lock_guard lk(g_mu);
  g_sink = std::move(sink);
}

void set_request_id(std::string id) { t_request_id = std::move(id); }
const std::string& request_id() { return t_request_id; }
void clear_request_id() { t_request_id.clear(); }

ScopedRequestId::ScopedRequestId(std::string id) : prev_(std::move(t_request_id)) {
  t_request_id = std::move(id);
}
ScopedRequestId::~ScopedRequestId() { t_request_id = std::move(prev_); }

}  // namespace azm::log
