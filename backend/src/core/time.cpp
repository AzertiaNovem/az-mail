#include "core/time.hpp"

#include "core/strings.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

namespace azm {
namespace {

constexpr int64_t kMsPerDay = 86'400'000;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// Reads exactly n digits at pos; advances pos.
bool read_digits(std::string_view s, std::size_t& pos, std::size_t n, int& out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    char c = s[pos + i];
    if (!is_digit(c)) return false;
    v = v * 10 + (c - '0');
  }
  pos += n;
  out = v;
  return true;
}

// Parses ±HH, ±HH:MM, ±HHMM starting at pos (at the sign). Returns offset minutes east.
bool parse_numeric_offset(std::string_view s, std::size_t& pos, int& minutes) {
  if (pos >= s.size() || (s[pos] != '+' && s[pos] != '-')) return false;
  const int sign = s[pos] == '-' ? -1 : 1;
  ++pos;
  int hh = 0, mm = 0;
  if (!read_digits(s, pos, 2, hh)) return false;
  if (pos < s.size() && s[pos] == ':') {
    ++pos;
    if (!read_digits(s, pos, 2, mm)) return false;
  } else if (pos + 2 <= s.size() && is_digit(s[pos]) && is_digit(s[pos + 1])) {
    read_digits(s, pos, 2, mm);
  }
  if (hh > 23 || mm > 59) return false;
  minutes = sign * (hh * 60 + mm);
  return true;
}

}  // namespace

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const Clock& system_clock() {
  static const SystemClock clock;
  return clock;
}

// Howard Hinnant's days_from_civil / civil_from_days (public domain algorithms).
int64_t days_from_civil(int64_t y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;                                   // [0, 399]
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
  return era * 146097 + doe - 719468;
}

CivilTime civil_from_ms(int64_t ms) {
  int64_t days = ms / kMsPerDay;
  int64_t rem = ms % kMsPerDay;
  if (rem < 0) {
    rem += kMsPerDay;
    --days;
  }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  CivilTime t;
  t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  t.year = static_cast<int>(yoe + era * 400 + (t.month <= 2 ? 1 : 0));
  t.hour = static_cast<int>(rem / 3'600'000);
  t.minute = static_cast<int>(rem / 60'000 % 60);
  t.second = static_cast<int>(rem / 1000 % 60);
  t.millisecond = static_cast<int>(rem % 1000);
  return t;
}

int days_in_month(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

std::optional<int64_t> utc_ms(int year, int month, int day, int hour, int minute, int second,
                              int millisecond) {
  if (year < 0 || year > 9999 || month < 1 || month > 12) return std::nullopt;
  if (day < 1 || day > days_in_month(year, month)) return std::nullopt;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
    return std::nullopt;
  if (millisecond < 0 || millisecond > 999) return std::nullopt;
  return days_from_civil(year, month, day) * kMsPerDay +
         ((hour * 60LL + minute) * 60LL + second) * 1000LL + millisecond;
}

std::string iso8601_utc(int64_t ms) {
  const CivilTime t = civil_from_ms(ms);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.year, t.month, t.day,
                t.hour, t.minute, t.second, t.millisecond);
  return buf;
}

std::optional<int64_t> parse_iso8601_lenient(std::string_view in) {
  const std::string_view s = trim(in);
  std::size_t p = 0;
  int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0, ms = 0;
  if (!read_digits(s, p, 4, y)) return std::nullopt;
  if (p >= s.size() || s[p] != '-') return std::nullopt;
  ++p;
  if (!read_digits(s, p, 2, mo)) return std::nullopt;
  if (p >= s.size() || s[p] != '-') return std::nullopt;
  ++p;
  if (!read_digits(s, p, 2, d)) return std::nullopt;

  int offset_min = 0;
  if (p < s.size()) {
    if (s[p] != 'T' && s[p] != 't' && s[p] != ' ') return std::nullopt;
    ++p;
    if (!read_digits(s, p, 2, hh)) return std::nullopt;
    if (p >= s.size() || s[p] != ':') return std::nullopt;
    ++p;
    if (!read_digits(s, p, 2, mi)) return std::nullopt;
    if (p < s.size() && s[p] == ':') {
      ++p;
      if (!read_digits(s, p, 2, ss)) return std::nullopt;
      if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
        ++p;
        std::size_t digits = 0;
        int frac = 0;
        while (p < s.size() && is_digit(s[p])) {
          if (digits < 3) frac = frac * 10 + (s[p] - '0');
          ++digits;
          ++p;
        }
        if (digits == 0) return std::nullopt;
        for (std::size_t i = digits; i < 3; ++i) frac *= 10;
        ms = frac;
      }
    }
    // Zone (optionally preceded by one space).
    if (p < s.size() && s[p] == ' ') ++p;
    if (p < s.size()) {
      const std::string_view rest = s.substr(p);
      if (rest == "Z" || rest == "z" || iequals(rest, "UTC") || iequals(rest, "GMT")) {
        p = s.size();
      } else if (!parse_numeric_offset(s, p, offset_min)) {
        return std::nullopt;
      }
    }
    if (p != s.size()) return std::nullopt;
  }
  auto base = utc_ms(y, mo, d, hh, mi, ss, ms);
  if (!base) return std::nullopt;
  return *base - static_cast<int64_t>(offset_min) * 60'000;
}

namespace {

std::optional<int> month_from_name(std::string_view t) {
  static constexpr std::string_view kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                                 "jul", "aug", "sep", "oct", "nov", "dec"};
  if (t.size() < 3) return std::nullopt;
  for (int i = 0; i < 12; ++i)
    if (iequals(t.substr(0, 3), kMonths[i])) return i + 1;
  return std::nullopt;
}

bool is_weekday_name(std::string_view t) {
  static constexpr std::string_view kDays[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  if (t.size() < 3) return false;
  for (auto d : kDays)
    if (iequals(t.substr(0, 3), d)) return true;
  return false;
}

bool all_digits(std::string_view t) {
  if (t.empty()) return false;
  for (char c : t)
    if (!is_digit(c)) return false;
  return true;
}

bool all_alpha(std::string_view t) {
  if (t.empty()) return false;
  for (char c : t)
    if (!is_alpha(c)) return false;
  return true;
}

// Obsolete zone names (RFC 5322 §4.3) → minutes east of UTC.
std::optional<int> obsolete_zone(std::string_view z) {
  struct Z {
    std::string_view name;
    int minutes;
  };
  static constexpr Z kZones[] = {{"UT", 0},         {"UTC", 0},        {"GMT", 0},
                                 {"Z", 0},          {"EST", -5 * 60},  {"EDT", -4 * 60},
                                 {"CST", -6 * 60},  {"CDT", -5 * 60},  {"MST", -7 * 60},
                                 {"MDT", -6 * 60},  {"PST", -8 * 60},  {"PDT", -7 * 60}};
  for (const auto& e : kZones)
    if (iequals(z, e.name)) return e.minutes;
  return std::nullopt;
}

}  // namespace

std::optional<int64_t> parse_rfc5322_date(std::string_view in) {
  // Drop comments (possibly nested, with quoted-pairs) and turn commas into spaces.
  std::string cleaned;
  cleaned.reserve(in.size());
  int depth = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (depth > 0) {
      if (c == '\\') {
        ++i;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')') {
        --depth;
        if (depth == 0) cleaned.push_back(' ');
      }
      continue;
    }
    if (c == '(') {
      ++depth;
    } else if (c == ',') {
      cleaned.push_back(' ');
    } else {
      cleaned.push_back(is_space(c) ? ' ' : c);
    }
  }

  std::vector<std::string_view> tok;
  {
    std::string_view s = cleaned;
    std::size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && s[i] == ' ') ++i;
      std::size_t j = i;
      while (j < s.size() && s[j] != ' ') ++j;
      if (j > i) tok.push_back(s.substr(i, j - i));
      i = j;
    }
  }

  auto fallback = [&]() { return parse_iso8601_lenient(in); };

  std::size_t k = 0;
  if (k < tok.size() && all_alpha(tok[k]) && is_weekday_name(tok[k])) ++k;
  if (tok.size() < k + 4) return fallback();  // day, month, year, time

  int day = 0, month = 0, year = 0;
  // Standard order: day month year. Tolerate "month day year" as written by some mailers.
  if (all_digits(tok[k]) && tok[k].size() <= 2 && month_from_name(tok[k + 1])) {
    day = std::stoi(std::string(tok[k]));
    month = *month_from_name(tok[k + 1]);
  } else if (month_from_name(tok[k]) && all_digits(tok[k + 1]) && tok[k + 1].size() <= 2) {
    month = *month_from_name(tok[k]);
    day = std::stoi(std::string(tok[k + 1]));
  } else {
    return fallback();
  }
  k += 2;
  if (!all_digits(tok[k]) || tok[k].size() < 2 || tok[k].size() > 4) return fallback();
  year = std::stoi(std::string(tok[k]));
  if (tok[k].size() == 2) year += year < 50 ? 2000 : 1900;
  else if (tok[k].size() == 3) year += 1900;
  ++k;

  int hh = 0, mi = 0, ss = 0;
  {
    const std::string_view t = tok[k];
    std::size_t p = 0;
    // Hour is 2 digits per RFC; tolerate a single digit ("7:03").
    if (!read_digits(t, p, 2, hh) && !read_digits(t, p, 1, hh)) return fallback();
    if (p >= t.size() || t[p] != ':') return fallback();
    ++p;
    if (!read_digits(t, p, 2, mi)) return fallback();
    if (p < t.size() && t[p] == ':') {
      ++p;
      if (!read_digits(t, p, 2, ss)) return fallback();
    }
    if (p != t.size()) return fallback();
    ++k;
  }

  int offset_min = 0;
  if (k < tok.size()) {
    const std::string_view z = tok[k];
    if (z[0] == '+' || z[0] == '-') {
      std::size_t p = 0;
      if (!parse_numeric_offset(z, p, offset_min) || p != z.size()) offset_min = 0;
    } else if (auto oz = obsolete_zone(z)) {
      offset_min = *oz;
    } else {
      offset_min = 0;  // military letters and unknown zones: treat as UTC (-0000)
    }
  }

  auto base = utc_ms(year, month, day, hh, mi, ss, 0);
  if (!base) return std::nullopt;
  return *base - static_cast<int64_t>(offset_min) * 60'000;
}

}  // namespace azm
