// JSON parsing with limits and typed field getters that raise ApiError(400).
//   parse errors      → ApiError(400, "invalid_json")
//   wrong/missing field → ApiError(400, "invalid_field", details {"field": "<name>"})
// Missing keys and explicit null both count as "absent" for the opt_* getters.
#pragma once

#include "core/errors.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azm {

boost::json::value parse_json(std::string_view text, std::size_t max_depth = 64);
// Same, but the top-level value must be an object (else invalid_json).
boost::json::object parse_json_object(std::string_view text, std::size_t max_depth = 64);

std::string serialize_json(const boost::json::value& v);

// Throws ApiError(400, "invalid_field", {field}).
[[noreturn]] void throw_invalid_field(std::string_view field);

std::string req_string(const boost::json::object& o, std::string_view key);
std::optional<std::string> opt_string(const boost::json::object& o, std::string_view key);

// Integers: JSON int64/uint64 within range, or a double with an exact integral value.
int64_t req_int64(const boost::json::object& o, std::string_view key);
std::optional<int64_t> opt_int64(const boost::json::object& o, std::string_view key);

double req_double(const boost::json::object& o, std::string_view key);
std::optional<double> opt_double(const boost::json::object& o, std::string_view key);

bool req_bool(const boost::json::object& o, std::string_view key);
std::optional<bool> opt_bool(const boost::json::object& o, std::string_view key);

// Containers: pointer into `o` (nullptr when absent/null); wrong type throws invalid_field.
const boost::json::array* opt_array(const boost::json::object& o, std::string_view key);
const boost::json::object* opt_object(const boost::json::object& o, std::string_view key);
const boost::json::array& req_array(const boost::json::object& o, std::string_view key);
const boost::json::object& req_object(const boost::json::object& o, std::string_view key);

// Arrays of ids, e.g. {"thread_ids":[1,2,3]}. Each element must be an integer.
std::vector<int64_t> req_int64_array(const boost::json::object& o, std::string_view key);
std::optional<std::vector<int64_t>> opt_int64_array(const boost::json::object& o,
                                                    std::string_view key);
std::optional<std::vector<std::string>> opt_string_array(const boost::json::object& o,
                                                         std::string_view key);

// Value → integer helper used by the getters (nullopt if not an exact int64).
std::optional<int64_t> as_int64(const boost::json::value& v);

// optional<T> → JSON value (null when empty).
template <class T>
boost::json::value to_json(const std::optional<T>& v) {
  if (!v) return nullptr;
  return boost::json::value(*v);
}
inline boost::json::value to_json(const std::optional<std::string_view>& v) {
  if (!v) return nullptr;
  return boost::json::value(boost::json::string(*v));
}

}  // namespace azm
