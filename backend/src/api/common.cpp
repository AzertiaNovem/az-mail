// Owner: WP-D
#include "api/common.hpp"

#include "core/crypto.hpp"
#include "core/log.hpp"

#include <algorithm>

namespace azm::api::detail {

namespace {

struct FallbackGate {
  std::mutex mu;
  std::condition_variable cv;
  int in_use = 0;
};

FallbackGate& fallback_gate() {
  static FallbackGate g;
  return g;
}

}  // namespace

ScryptSlot::ScryptSlot(const Services& svc) {
  if (svc.login_throttle != nullptr) {
    permit_.emplace(svc.login_throttle->acquire_scrypt());
    return;
  }
  const int limit = std::max(1, svc.cfg.scrypt_concurrency);
  auto& g = fallback_gate();
  std::unique_lock lk(g.mu);
  g.cv.wait(lk, [&] { return g.in_use < limit; });
  ++g.in_use;
  fallback_ = true;
}

ScryptSlot::~ScryptSlot() {
  if (!fallback_) return;  // permit_ releases itself
  auto& g = fallback_gate();
  {
    std::lock_guard lk(g.mu);
    --g.in_use;
  }
  g.cv.notify_one();
}

const std::string& dummy_password_hash() {
  static const std::string h = crypto::password_hash("azmail-timing-equalizer-password");
  return h;
}

std::string hash_password(const Services& svc, std::string_view password) {
  ScryptSlot slot(svc);
  return crypto::password_hash(password);
}

void throw_blob_error(const BlobError& e, std::string_view op) {
  log::warn("blob store error", {{"op", op}, {"retryable", e.retryable}, {"error", e.what()}});
  if (e.retryable)
    throw ApiError::unavailable("storage_unavailable", "文件存储暂时不可用，请稍后重试");
  throw ApiError::bad_gateway("storage_error", "文件存储出错");
}

void throw_resend_error(const resend::Error& e, std::string_view op) {
  log::warn("resend call failed", {{"op", op},
                                   {"kind", resend::to_string(e.kind)},
                                   {"status", e.http_status},
                                   {"name", e.name}});
  throw ApiError::bad_gateway("resend_error", "邮件服务请求失败，请稍后重试");
}

const http::Principal& require_admin(const http::Ctx& ctx) {
  const auto& p = ctx.user();
  if (!p.is_admin) throw ApiError::forbidden();
  return p;
}

std::optional<std::string> query_nonempty(const http::Ctx& ctx, std::string_view key) {
  auto v = ctx.query(key);
  if (!v || v->empty()) return std::nullopt;
  return std::string(*v);
}

bool query_flag(const http::Ctx& ctx, std::string_view key, bool def) {
  auto v = ctx.query(key);
  if (!v || v->empty()) return def;
  if (*v == "1" || *v == "true") return true;
  if (*v == "0" || *v == "false") return false;
  throw ApiError::bad_request("invalid_field", "参数无效", field_detail(key));
}

boost::json::object field_detail(std::string_view field) {
  boost::json::object d;
  d["field"] = field;
  return d;
}

}  // namespace azm::api::detail
