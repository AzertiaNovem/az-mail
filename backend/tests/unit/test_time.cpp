#include "core/time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace azm;

namespace {
constexpr int64_t kOct7Noon = 1791374400000;  // 2026-10-07T12:00:00.000Z
constexpr int64_t kOct7_1203 = 1791374580000;  // 2026-10-07T12:03:00.000Z
}  // namespace

TEST_CASE("iso8601_utc formatting", "[time]") {
  CHECK(iso8601_utc(0) == "1970-01-01T00:00:00.000Z");
  CHECK(iso8601_utc(kOct7Noon) == "2026-10-07T12:00:00.000Z");
  CHECK(iso8601_utc(kOct7Noon + 123) == "2026-10-07T12:00:00.123Z");
  CHECK(iso8601_utc(-1) == "1969-12-31T23:59:59.999Z");
  CHECK(iso8601_utc(1709164800000) == "2024-02-29T00:00:00.000Z");
  CHECK(iso8601_utc(946684799000) == "1999-12-31T23:59:59.000Z");
}

TEST_CASE("civil helpers", "[time]") {
  CHECK(days_from_civil(1970, 1, 1) == 0);
  CHECK(days_from_civil(2000, 3, 1) == 11017);
  const auto t = civil_from_ms(kOct7Noon + 4321);
  CHECK(t.year == 2026);
  CHECK(t.month == 10);
  CHECK(t.day == 7);
  CHECK(t.hour == 12);
  CHECK(t.second == 4);
  CHECK(t.millisecond == 321);
  CHECK(days_in_month(2024, 2) == 29);
  CHECK(days_in_month(2100, 2) == 28);
  CHECK(days_in_month(2000, 2) == 29);
  CHECK(utc_ms(2026, 10, 7, 12).value() == kOct7Noon);
  CHECK_FALSE(utc_ms(2026, 2, 29).has_value());
  CHECK_FALSE(utc_ms(2026, 13, 1).has_value());
}

TEST_CASE("parse_iso8601_lenient accepts common forms", "[time]") {
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00.000Z") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00Z") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07t12:00:00z") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07 12:00:00") == kOct7Noon);  // no zone = UTC
  CHECK(parse_iso8601_lenient("  2026-10-07T12:00:00Z \n") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00Z") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07") == kOct7Noon - 12 * 3600 * 1000);
  CHECK(parse_iso8601_lenient("2026-10-07 12:00:00 UTC") == kOct7Noon);
}

TEST_CASE("parse_iso8601_lenient offsets and fractions", "[time]") {
  CHECK(parse_iso8601_lenient("2026-10-07T20:00:00+08:00") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T20:00:00+0800") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T20:00:00+08") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T07:00:00-05:00") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T17:30:00+05:30") == kOct7Noon);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00 +0000") == kOct7Noon);
  // Resend / Postgres style: space separator, microseconds, "+00".
  CHECK(parse_iso8601_lenient("2026-04-03 22:13:42.674981+00") == 1775254422674);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00.5Z") == kOct7Noon + 500);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00.05Z") == kOct7Noon + 50);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00.123456789Z") == kOct7Noon + 123);
  CHECK(parse_iso8601_lenient("2026-10-07T12:00:00,250Z") == kOct7Noon + 250);
  CHECK(parse_iso8601_lenient("2024-02-29T00:00:00Z") == 1709164800000);
}

TEST_CASE("parse_iso8601_lenient rejects garbage", "[time]") {
  CHECK_FALSE(parse_iso8601_lenient(""));
  CHECK_FALSE(parse_iso8601_lenient("garbage"));
  CHECK_FALSE(parse_iso8601_lenient("2026-13-01T00:00:00Z"));
  CHECK_FALSE(parse_iso8601_lenient("2026-02-29T00:00:00Z"));
  CHECK_FALSE(parse_iso8601_lenient("2026-10-07T25:00:00Z"));
  CHECK_FALSE(parse_iso8601_lenient("2026-10-07T12:60:00Z"));
  CHECK_FALSE(parse_iso8601_lenient("2026-10-07T12:00:00+2500"));
  CHECK_FALSE(parse_iso8601_lenient("2026-10-07T12:00:00.Z"));
  CHECK_FALSE(parse_iso8601_lenient("2026-10-07T12:00:00Zjunk"));
  CHECK_FALSE(parse_iso8601_lenient("2026/10/07 12:00:00"));
  CHECK_FALSE(parse_iso8601_lenient("in 5 minutes"));
}

TEST_CASE("iso8601 roundtrip", "[time]") {
  for (int64_t ms : {int64_t{0}, kOct7Noon, kOct7Noon + 999, int64_t{1775254422674}, int64_t{253402300799999}}) {
    CHECK(parse_iso8601_lenient(iso8601_utc(ms)) == ms);
  }
}

TEST_CASE("parse_rfc5322_date", "[time]") {
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 20:03:00 +0800") == kOct7_1203);
  CHECK(parse_rfc5322_date("7 Oct 2026 12:03:00 GMT") == kOct7_1203);
  CHECK(parse_rfc5322_date("Tue, 07 Oct 2026 08:03:00 -0400 (EDT)") == kOct7_1203);
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 07:03 EST") == kOct7_1203);  // no seconds
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 05:03:00 PDT") == kOct7_1203);
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 12:03:00 UT") == kOct7_1203);
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 12:03:00 Z") == kOct7_1203);       // military
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 12:03:00 XYZ") == kOct7_1203);     // unknown → UTC
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 12:03:00") == kOct7_1203);         // no zone
  CHECK(parse_rfc5322_date("7 Oct 26 12:03:00 +0000") == kOct7_1203);          // 2-digit year
  CHECK(parse_rfc5322_date("Tue,\r\n 7 Oct 2026 12:03:00 +0000") == kOct7_1203);  // folded
  CHECK(parse_rfc5322_date("Tuesday, 7 October 2026 12:03:00 +0000") == kOct7_1203);
  CHECK(parse_rfc5322_date("Tue, 7 Oct 2026 12:03:00 +0000 (Coordinated (Universal) Time)") ==
        kOct7_1203);
  CHECK(parse_rfc5322_date("Tue Oct 7 2026 12:03:00 +0000") == kOct7_1203);  // month-first
  CHECK(parse_rfc5322_date("2026-10-07T12:03:00Z") == kOct7_1203);           // ISO fallback
  CHECK(parse_rfc5322_date("Fri, 1 Jan 99 00:00:00 +0000") == 915148800000);  // 1999

  CHECK_FALSE(parse_rfc5322_date("not a date"));
  CHECK_FALSE(parse_rfc5322_date(""));
  CHECK_FALSE(parse_rfc5322_date("32 Oct 2026 12:00:00 +0000"));
  CHECK_FALSE(parse_rfc5322_date("7 Foo 2026 12:00:00 +0000"));
  CHECK_FALSE(parse_rfc5322_date("7 Oct 2026"));
}

TEST_CASE("clocks", "[time]") {
  ManualClock mc(1000);
  const Clock& c = mc;
  CHECK(c.now_ms() == 1000);
  mc.advance(500);
  CHECK(c.now_ms() == 1500);
  mc.set(42);
  CHECK(c.now_ms() == 42);

  const int64_t sys = system_clock().now_ms();
  CHECK(std::llabs(sys - now_ms()) < 5000);
  CHECK(sys > kOct7Noon - 400LL * 86400 * 1000);  // sanity: after 2025
}
