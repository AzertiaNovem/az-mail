// Owner: WP-D
// Uploads and signed file downloads (DESIGN D4, D5, Addendum A data flows; CONTRACTS §B).
//  * Upload: the session streamed the body to BlobStore::tmp_dir() (req.body_file, sha256 while
//    streaming); here put_file (files pool, outside any tx) → one short tx inserts blobs +
//    attachments. blob_writer_guard() is held from put_file until that tx committed (§H 23).
//  * Download: HMAC signed URL (no Bearer) → user active → ownership → serve() plan: local file
//    (nosniff, RFC 6266, sandbox CSP for unsafe types) or a presigned R2 redirect.
#include "api/common.hpp"
#include "api/handlers.hpp"
#include "core/blob_store.hpp"
#include "core/errors.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "db/sqlite.hpp"
#include "mail/attachments.hpp"
#include "mail/render.hpp"
#include "mail/serde.hpp"
#include "repo/accounts.hpp"
#include "services.hpp"

#include <charconv>
#include <chrono>
#include <variant>

namespace azm::api {

namespace {

constexpr std::size_t kMaxFilenameParam = 1024;  // bytes, before mail::create_upload sanitizes

[[noreturn]] void invalid_signature() {
  throw ApiError::forbidden("invalid_signature", "链接无效或已过期");
}

std::optional<int64_t> strict_i64(std::optional<std::string_view> s) {
  if (!s || s->empty()) return std::nullopt;
  int64_t v = 0;
  auto [p, ec] = std::from_chars(s->data(), s->data() + s->size(), v);
  if (ec != std::errc() || p != s->data() + s->size()) return std::nullopt;
  return v;
}

struct SignedParams {
  int64_t user_id = 0;
  int64_t exp_ms = 0;
  std::string sig;
};

// u, exp, sig of a signed URL; any malformed value is reported as an invalid signature.
SignedParams signed_params(const http::Ctx& ctx) {
  SignedParams p;
  const auto u = strict_i64(ctx.query("u"));
  const auto exp = strict_i64(ctx.query("exp"));
  const auto sig = ctx.query("sig");
  if (!u || *u <= 0 || !exp || !sig || sig->empty()) invalid_signature();
  p.user_id = *u;
  p.exp_ms = *exp;
  p.sig = std::string(*sig);
  return p;
}

// The URL's user must still exist and be active (D5): disabled users lose file access at once.
void require_active_user(db::Conn& c, int64_t user_id) {
  auto u = repo::get_user(c, user_id);
  if (!u || u->disabled) invalid_signature();
}

// Serves a blob per its ServePlan. `local_cache` is the Cache-Control for streamed files.
http::Response serve_blob(Services& svc, std::string_view storage, std::string_view sha,
                          const ServeOptions& opts, const std::string& local_type, bool sandbox,
                          std::string_view op) {
  try {
    BlobStore& store = svc.blobs_for(storage);
    const ServePlan plan = store.serve(sha, opts);
    if (const auto* local = std::get_if<LocalFile>(&plan)) {
      auto r = http::Response::file(local->path, local_type, opts.disposition);
      r.add_header("X-Content-Type-Options", "nosniff");
      if (sandbox) r.add_header("Content-Security-Policy", "sandbox");
      r.add_header("Cache-Control", "private, max-age=3600");
      return r;
    }
    auto r = http::Response::redirect(std::get<RedirectUrl>(plan).url);
    r.add_header("Cache-Control", "private, max-age=60");
    return r;
  } catch (const BlobNotFound&) {
    throw ApiError::not_found("not_found", "文件不存在");
  } catch (const BlobError& e) {
    detail::throw_blob_error(e, op);
  }
}

}  // namespace

// POST /api/attachments?filename=<pct-enc>&inline=0|1 (file body, files pool) → 201 Attachment
http::Response attachments_upload(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const int64_t owner = ctx.user().user_id;

  // req.query is already percent-decoded once by the session (http/types.hpp).
  const auto raw_name = ctx.query("filename");
  if (!raw_name || trim(*raw_name).empty() || raw_name->size() > kMaxFilenameParam)
    throw ApiError::bad_request("invalid_field", "缺少或无效的文件名", detail::field_detail("filename"));
  const std::string filename = utf8_sanitize(trim(*raw_name));
  const bool is_inline = detail::query_flag(ctx, "inline", false);
  const std::string content_type(ctx.req.header("Content-Type").value_or("application/octet-stream"));
  if (!ctx.req.body_file) throw ApiError::bad_request("bad_request", "缺少上传内容");
  if (ctx.req.body_size > svc.cfg.upload_body_limit)
    throw ApiError::too_large("payload_too_large", "文件超过 25 MiB 上限");

  // Held until create_upload committed, so gc.blobs cannot delete the object in between.
  auto guard = blob_writer_guard();
  BlobRef blob;
  try {
    blob = svc.blobs.put_file(*ctx.req.body_file, ctx.req.body_sha256);
  } catch (const BlobError& e) {
    detail::throw_blob_error(e, "upload");
  }
  const int64_t now = svc.now_ms();
  const auto view = svc.db.write([&](db::Tx& tx) {
    return mail::create_upload(tx, svc.signed_urls, owner, blob, filename, content_type, is_inline, now);
  });
  return http::Response::json(mail::to_json(view), 201);
}

// GET  /api/files/:id?d=i|a&u&exp&sig (signed, files pool)
http::Response files_get(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const int64_t id = ctx.id("id");
  const auto d = ctx.query("d");
  if (!d || (*d != "i" && *d != "a")) invalid_signature();
  const char disp = (*d)[0];
  const SignedParams sp = signed_params(ctx);
  if (!svc.signed_urls.verify_file(id, sp.user_id, disp, sp.exp_ms, sp.sig, svc.now_ms()))
    invalid_signature();

  const auto att = svc.db.read([&](db::Conn& c) {
    require_active_user(c, sp.user_id);
    return mail::find_attachment(c, sp.user_id, id);
  });
  if (!att) throw ApiError::not_found("not_found", "文件不存在");

  const auto policy = mail::file_serve_policy(att->content_type, att->filename, disp);
  ServeOptions opts{policy.redirect_content_type, policy.disposition,
                    std::chrono::seconds(svc.cfg.r2_presign_ttl_sec)};
  return serve_blob(svc, att->storage, att->blob_sha256, opts, policy.content_type, policy.sandbox_csp,
                    "files_get");
}

// GET  /api/files/raw/:messageId?u&exp&sig (signed, files pool) → message/rfc822 attachment
http::Response files_raw(http::Ctx& ctx) {
  Services& svc = ctx.svc;
  const int64_t id = ctx.id("messageId");
  const SignedParams sp = signed_params(ctx);
  if (!svc.signed_urls.verify_raw(id, sp.user_id, sp.exp_ms, sp.sig, svc.now_ms())) invalid_signature();

  const auto raw = svc.db.read([&](db::Conn& c) {
    require_active_user(c, sp.user_id);
    return mail::find_raw(c, sp.user_id, id);
  });
  if (!raw) throw ApiError::not_found("not_found", "原始邮件不存在");

  const std::string type = "message/rfc822";
  ServeOptions opts{type, content_disposition("attachment", raw->filename),
                    std::chrono::seconds(svc.cfg.r2_presign_ttl_sec)};
  return serve_blob(svc, raw->storage, raw->sha256, opts, type, true, "files_raw");
}

}  // namespace azm::api
