#include "core/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

using namespace azm;
namespace bj = boost::json;

namespace {

// Runs f and returns the ApiError it throws (fails the test if none).
template <class F>
ApiError catch_api(F&& f) {
  try {
    f();
  } catch (const ApiError& e) {
    return e;
  }
  FAIL("expected ApiError");
  return ApiError(0, "", "");
}

}  // namespace

TEST_CASE("parse_json valid and invalid", "[json]") {
  const bj::value v = parse_json(R"({"a":1,"b":[true,null],"c":"中文"})");
  REQUIRE(v.is_object());
  CHECK(v.as_object().at("c").as_string() == "中文");

  const auto e = catch_api([] { parse_json("{\"a\":"); });
  CHECK(e.status == 400);
  CHECK(e.code == "invalid_json");
  CHECK(catch_api([] { parse_json("{} trailing"); }).code == "invalid_json");
  CHECK(catch_api([] { parse_json(""); }).code == "invalid_json");
  CHECK(catch_api([] { parse_json("{\"a\":\"\xff\"}"); }).code == "invalid_json");  // bad UTF-8
  CHECK(catch_api([] { parse_json("{'a':1}"); }).code == "invalid_json");
}

TEST_CASE("parse_json depth limit", "[json]") {
  const std::string deep = std::string(100, '[') + std::string(100, ']');
  CHECK(catch_api([&] { parse_json(deep); }).code == "invalid_json");
  CHECK_NOTHROW(parse_json(deep, 128));
  const std::string ok = std::string(10, '[') + std::string(10, ']');
  CHECK_NOTHROW(parse_json(ok));
}

TEST_CASE("parse_json_object requires an object", "[json]") {
  CHECK(parse_json_object("{\"x\":1}").at("x").as_int64() == 1);
  CHECK(catch_api([] { parse_json_object("[1,2]"); }).code == "invalid_json");
  CHECK(catch_api([] { parse_json_object("\"s\""); }).code == "invalid_json");
}

TEST_CASE("string getters", "[json]") {
  const auto o = parse_json_object(R"({"s":"v","n":1,"z":null})");
  CHECK(req_string(o, "s") == "v");
  CHECK(opt_string(o, "s") == "v");
  CHECK_FALSE(opt_string(o, "missing"));
  CHECK_FALSE(opt_string(o, "z"));  // null counts as absent

  const auto e = catch_api([&] { req_string(o, "missing"); });
  CHECK(e.status == 400);
  CHECK(e.code == "invalid_field");
  CHECK(e.details.at("field").as_string() == "missing");
  CHECK(catch_api([&] { req_string(o, "n"); }).details.at("field").as_string() == "n");
  CHECK(catch_api([&] { opt_string(o, "n"); }).code == "invalid_field");
}

TEST_CASE("integer getters", "[json]") {
  const auto o = parse_json_object(
      R"({"i":-5,"u":18446744073709551615,"d":3.0,"f":3.5,"s":"1","big":9007199254740993,"t":true})");
  CHECK(req_int64(o, "i") == -5);
  CHECK(req_int64(o, "d") == 3);
  CHECK(req_int64(o, "big") == 9007199254740993LL);
  CHECK_FALSE(opt_int64(o, "missing"));
  CHECK(catch_api([&] { req_int64(o, "u"); }).code == "invalid_field");  // > INT64_MAX
  CHECK(catch_api([&] { req_int64(o, "f"); }).code == "invalid_field");
  CHECK(catch_api([&] { req_int64(o, "s"); }).code == "invalid_field");
  CHECK(catch_api([&] { req_int64(o, "t"); }).code == "invalid_field");
  CHECK(catch_api([&] { req_int64(o, "missing"); }).code == "invalid_field");
  CHECK(req_double(o, "f") == 3.5);
  CHECK(opt_double(o, "i") == -5.0);
}

TEST_CASE("bool / container getters", "[json]") {
  const auto o = parse_json_object(R"({"b":false,"a":[1,2,3],"o":{"k":1},"mixed":[1,"x"],"sa":["x","y"]})");
  CHECK(req_bool(o, "b") == false);
  CHECK_FALSE(opt_bool(o, "missing"));
  CHECK(catch_api([&] { req_bool(o, "a"); }).code == "invalid_field");

  REQUIRE(opt_array(o, "a") != nullptr);
  CHECK(opt_array(o, "a")->size() == 3);
  CHECK(opt_array(o, "missing") == nullptr);
  CHECK(catch_api([&] { opt_array(o, "o"); }).code == "invalid_field");
  REQUIRE(opt_object(o, "o") != nullptr);
  CHECK(req_object(o, "o").at("k").as_int64() == 1);
  CHECK(catch_api([&] { req_object(o, "a"); }).code == "invalid_field");
  CHECK(req_array(o, "a").size() == 3);

  CHECK(req_int64_array(o, "a") == std::vector<int64_t>{1, 2, 3});
  CHECK_FALSE(opt_int64_array(o, "missing"));
  CHECK(catch_api([&] { req_int64_array(o, "mixed"); }).code == "invalid_field");
  CHECK(opt_string_array(o, "sa").value() == std::vector<std::string>{"x", "y"});
  CHECK(catch_api([&] { opt_string_array(o, "a"); }).code == "invalid_field");
}

TEST_CASE("to_json for optionals and ApiError shape", "[json]") {
  CHECK(to_json(std::optional<std::string>{}).is_null());
  CHECK(to_json(std::optional<std::string>{"x"}).as_string() == "x");
  CHECK(to_json(std::optional<int64_t>{7}).as_int64() == 7);
  CHECK(to_json(std::optional<bool>{true}).as_bool());
  CHECK(to_json(std::optional<std::string_view>{"sv"}).as_string() == "sv");

  bj::object d;
  d["retry_after"] = 30;
  const ApiError e = ApiError::too_many("too_many_attempts", "尝试次数过多", d);
  CHECK(e.status == 429);
  CHECK(std::string(e.what()) == "too_many_attempts");
  const bj::object j = e.to_json();
  CHECK(serialize_json(j) ==
        R"({"error":{"code":"too_many_attempts","message":"尝试次数过多","details":{"retry_after":30}}})");
  CHECK(ApiError::not_found().status == 404);
  CHECK(ApiError::conflict("version_conflict", "x").status == 409);
  CHECK(ApiError::unprocessable().status == 422);
  CHECK(ApiError::too_large("message_too_large", "x").status == 413);
  CHECK(ApiError::internal().status == 500);
}
