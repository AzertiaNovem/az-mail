// Time helpers. All persisted / wire times are int64 milliseconds since the Unix epoch, UTC.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azm {

// Wall-clock now in ms epoch UTC (std::chrono::system_clock).
int64_t now_ms();

// Injectable clock so domain code / jobs can be tested deterministically.
struct Clock {
  virtual ~Clock() = default;
  virtual int64_t now_ms() const = 0;
};

struct SystemClock final : Clock {
  int64_t now_ms() const override { return azm::now_ms(); }
};

// Process-wide SystemClock instance.
const Clock& system_clock();

// Test clock: starts at `start_ms` and only moves when told to. Thread-safe.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(int64_t start_ms = 0) : ms_(start_ms) {}
  int64_t now_ms() const override { return ms_.load(std::memory_order_relaxed); }
  void set(int64_t ms) { ms_.store(ms, std::memory_order_relaxed); }
  void advance(int64_t delta_ms) { ms_.fetch_add(delta_ms, std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> ms_;
};

// ---- civil calendar (proleptic Gregorian, UTC) ---------------------------------------------
struct CivilTime {
  int year = 1970, month = 1, day = 1;  // month 1..12, day 1..31
  int hour = 0, minute = 0, second = 0, millisecond = 0;
};

// Days since 1970-01-01 for a civil date (no validation).
int64_t days_from_civil(int64_t y, int m, int d);
CivilTime civil_from_ms(int64_t ms);
// Validated civil → ms epoch (second may be 60 for leap seconds). nullopt when out of range.
std::optional<int64_t> utc_ms(int year, int month, int day, int hour = 0, int minute = 0,
                              int second = 0, int millisecond = 0);
int days_in_month(int year, int month);

// ---- formatting / parsing --------------------------------------------------------------------
// "2026-10-07T12:00:00.000Z"
std::string iso8601_utc(int64_t ms);

// Lenient ISO-8601 / RFC 3339 / Postgres timestamp parser:
//   YYYY-MM-DD[(T|t| )HH:MM[:SS[(.|,)frac]]][ ][Z|z|UTC|GMT|±HH|±HH:MM|±HHMM]
// A missing zone means UTC; fraction digits beyond milliseconds are truncated.
// Covers Resend's "2026-04-03 22:13:42.674981+00" and "2026-10-07T12:00:00.000Z".
std::optional<int64_t> parse_iso8601_lenient(std::string_view s);

// RFC 5322 Date header ("Tue, 7 Oct 2026 20:03:00 +0800"): optional weekday, comments,
// optional seconds, numeric or obsolete zones (UT, GMT, EST/EDT, CST/CDT, MST/MDT, PST/PDT,
// military letters → +0000, unknown → UTC), 2/3-digit years per RFC 5322 §4.3.
// Falls back to parse_iso8601_lenient for ISO-looking values written by broken mailers.
std::optional<int64_t> parse_rfc5322_date(std::string_view s);

}  // namespace azm
