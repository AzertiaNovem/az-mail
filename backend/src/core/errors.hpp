// API error type mapped to `{"error":{"code","message","details"}}` by http::dispatch.
#pragma once

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace azm {

// Thrown by handlers / domain code to produce a structured HTTP error.
// `code` is a stable snake_case identifier; `message` is user-facing Simplified Chinese.
struct ApiError : std::exception {
  unsigned status = 500;
  std::string code;
  std::string message;
  boost::json::object details;

  ApiError(unsigned status_, std::string code_, std::string message_,
           boost::json::object details_ = {})
      : status(status_),
        code(std::move(code_)),
        message(std::move(message_)),
        details(std::move(details_)) {}

  // what() is the code only: safe to log, never contains user data.
  const char* what() const noexcept override { return code.c_str(); }

  // {"error":{"code":..,"message":..,"details":{..}}}
  boost::json::object to_json() const {
    boost::json::object err;
    err["code"] = code;
    err["message"] = message;
    err["details"] = details;
    boost::json::object out;
    out["error"] = std::move(err);
    return out;
  }

  // ---- factories ---------------------------------------------------------
  static ApiError bad_request(std::string code = "bad_request", std::string message = "请求无效",
                              boost::json::object details = {}) {
    return {400, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError unauthorized(std::string code = "unauthorized",
                               std::string message = "未登录或登录已过期",
                               boost::json::object details = {}) {
    return {401, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError forbidden(std::string code = "forbidden", std::string message = "无权执行此操作",
                            boost::json::object details = {}) {
    return {403, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError not_found(std::string code = "not_found", std::string message = "资源不存在",
                            boost::json::object details = {}) {
    return {404, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError conflict(std::string code = "conflict", std::string message = "操作冲突",
                           boost::json::object details = {}) {
    return {409, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError too_large(std::string code = "payload_too_large",
                            std::string message = "请求内容过大",
                            boost::json::object details = {}) {
    return {413, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError unprocessable(std::string code = "unprocessable",
                                std::string message = "无法处理该请求",
                                boost::json::object details = {}) {
    return {422, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError too_many(std::string code = "too_many_requests",
                           std::string message = "请求过于频繁，请稍后再试",
                           boost::json::object details = {}) {
    return {429, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError internal(std::string code = "internal_error",
                           std::string message = "服务器内部错误",
                           boost::json::object details = {}) {
    return {500, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError bad_gateway(std::string code = "bad_gateway",
                              std::string message = "上游服务出错",
                              boost::json::object details = {}) {
    return {502, std::move(code), std::move(message), std::move(details)};
  }
  static ApiError unavailable(std::string code = "service_unavailable",
                              std::string message = "服务繁忙，请稍后再试",
                              boost::json::object details = {}) {
    return {503, std::move(code), std::move(message), std::move(details)};
  }
};

// Thrown by WP0 stubs that a later work package implements.
struct NotImplemented : std::logic_error {
  using std::logic_error::logic_error;
  NotImplemented() : std::logic_error("not_implemented") {}
};

}  // namespace azm
