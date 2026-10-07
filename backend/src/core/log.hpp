// Leveled, thread-safe logger writing one line per record to stderr (journald-friendly):
//   2026-10-07T12:00:00.000Z INFO [req-id] message key=value key2="quoted value"
// Never log secrets, tokens, signed-URL query strings or mail bodies.
#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace azm::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

// Parses "trace|debug|info|warn|warning|error|off" (case-insensitive).
std::optional<Level> parse_level(std::string_view s);
std::string_view level_name(Level l);  // "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"

void set_level(Level l);
Level level();
bool enabled(Level l);

// One structured key=value field. Values containing spaces, quotes, '=' or control
// characters are double-quoted with escapes.
struct Field {
  std::string_view key;
  std::string value;

  Field(std::string_view k, std::string_view v) : key(k), value(v) {}
  Field(std::string_view k, const char* v) : key(k), value(v ? v : "") {}
  Field(std::string_view k, const std::string& v) : key(k), value(v) {}
  Field(std::string_view k, bool v) : key(k), value(v ? "true" : "false") {}
  template <class T>
    requires(std::is_arithmetic_v<T> && !std::same_as<T, bool>)
  Field(std::string_view k, T v) : key(k), value(std::to_string(v)) {}
};

// Emit a record (no-op if the level is disabled).
void write(Level l, std::string_view msg, std::initializer_list<Field> fields = {});

inline void trace(std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Trace, m, f); }
inline void debug(std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Debug, m, f); }
inline void info(std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Info, m, f); }
inline void warn(std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Warn, m, f); }
inline void error(std::string_view m, std::initializer_list<Field> f = {}) { write(Level::Error, m, f); }

// Formats a record exactly as write() would (without the trailing newline). Exposed for tests.
std::string format_line(int64_t ts_ms, Level l, std::string_view request_id, std::string_view msg,
                        std::initializer_list<Field> fields);

// Replace the output sink (default: stderr). The sink receives full lines including '\n' and
// is called under the logger's mutex. Pass an empty function to restore stderr.
void set_sink(std::function<void(std::string_view line)> sink);

// ---- thread-local request id ---------------------------------------------------------------
void set_request_id(std::string id);
const std::string& request_id();  // "" when unset
void clear_request_id();

// RAII: sets the thread's request id for a scope and restores the previous one afterwards.
class ScopedRequestId {
 public:
  explicit ScopedRequestId(std::string id);
  ~ScopedRequestId();
  ScopedRequestId(const ScopedRequestId&) = delete;
  ScopedRequestId& operator=(const ScopedRequestId&) = delete;

 private:
  std::string prev_;
};

}  // namespace azm::log
