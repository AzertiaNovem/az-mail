// Owner: WP-C
//
// Pure AWS Signature Version 4 for the S3 API (Cloudflare R2; DESIGN Addendum A / A.2).
// No I/O, no clock: every input is explicit so the functions are unit-tested against the AWS
// published S3 examples listed in DESIGN A.2 (signing key dbb893ac…, GET /test.txt → f0e8bdb8…,
// PUT /test%24file.text → 98ad7217…, GET /?lifecycle → fea454ca…, GET /?max-keys=2&prefix=J →
// 34b48302…, presigned GET → aeeed9bb…).
// Encoding (A.2): URI-encode everything except A-Za-z0-9-._~ with uppercase hex, space → %20;
// '/' kept in the path and encoded as %2F in query values; query params sorted after encoding;
// the path is not normalized; canonical lines joined with '\n', a blank line after the header
// block, no trailing newline.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azm::storage::sigv4 {

inline constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
// sha256("") — x-amz-content-sha256 for GET / HEAD / DELETE.
inline constexpr std::string_view kEmptyPayloadSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
inline constexpr std::string_view kUnsignedPayload = "UNSIGNED-PAYLOAD";  // presigned URLs

using KeyValues = std::vector<std::pair<std::string, std::string>>;

struct Credentials {
  std::string access_key_id;
  std::string secret_access_key;  // never logged
};

// ---- building blocks -------------------------------------------------------------------------

// RFC 3986 unreserved kept; everything else %XX (uppercase). '/' kept iff !encode_slash.
std::string uri_encode(std::string_view s, bool encode_slash = true);
// Canonical URI for a RAW (unencoded) path, e.g. "/bucket/test$file.text" →
// "/bucket/test%24file.text"; empty → "/". Use the same string in the request URL.
std::string canonical_uri(std::string_view raw_path);
// Canonical query string from RAW name/value pairs: each encoded, sorted by encoded name then
// value, "k=v" joined with '&'; a key without value renders "k=". Empty input → "".
std::string canonical_query(std::span<const std::pair<std::string, std::string>> params);

struct CanonicalHeaders {
  std::string block;           // "name:value\n" per header (lowercased names, trimmed values,
                               // inner whitespace runs collapsed), sorted by name
  std::string signed_headers;  // "host;range;x-amz-content-sha256;x-amz-date"
};
// Every header given is signed. Duplicate names are joined with ','.
CanonicalHeaders canonical_headers(std::span<const std::pair<std::string, std::string>> headers);

// METHOD \n URI \n QUERY \n HEADERS-BLOCK \n SIGNED-HEADERS \n PAYLOAD-HASH
std::string canonical_request(std::string_view method, std::string_view canonical_uri,
                              std::string_view canonical_query, const CanonicalHeaders& headers,
                              std::string_view payload_sha256_hex);

// "YYYYMMDD/<region>/<service>/aws4_request"
std::string credential_scope(std::string_view date8, std::string_view region,
                             std::string_view service);

// "AWS4-HMAC-SHA256\n<amz_date>\n<scope>\n<hex sha256(canonical_request)>"
std::string string_to_sign(std::string_view amz_date, std::string_view scope,
                           std::string_view canonical_request);

// HMAC chain "AWS4"+secret → date8 → region → service → "aws4_request"; 32 raw bytes.
std::string signing_key(std::string_view secret_access_key, std::string_view date8,
                        std::string_view region, std::string_view service);

// Lowercase hex HMAC-SHA256(signing_key, string_to_sign).
std::string signature(std::string_view signing_key_raw, std::string_view string_to_sign);

// "AWS4-HMAC-SHA256 Credential=<akid>/<scope>, SignedHeaders=<sh>, Signature=<sig>"
std::string authorization_header(std::string_view access_key_id, std::string_view scope,
                                 std::string_view signed_headers, std::string_view signature_hex);

// "YYYYMMDDTHHMMSSZ" for a ms-epoch time (sub-second part dropped).
std::string amz_date(int64_t ms);

// ---- one-shot signing ----------------------------------------------------------------------

// A request to sign with an Authorization header. `headers` must contain every header to be
// signed, including host (with ":port" when non-default), x-amz-date and x-amz-content-sha256.
struct RequestToSign {
  std::string method;          // "GET", "PUT", "HEAD", "DELETE"
  std::string raw_path;        // unencoded, e.g. "/azmail-bucket/azmail/blobs/ab/cd/<sha>"
  KeyValues query;             // raw name/value pairs
  KeyValues headers;           // headers to sign (see above)
  std::string payload_sha256;  // hex sha256 of the body, kEmptyPayloadSha256, or kUnsignedPayload
  std::string amz_date;        // "20130524T000000Z" (its first 8 chars are the scope date)
  std::string region = "auto";  // R2: "auto"; AWS examples: "us-east-1"
  std::string service = "s3";
};

struct SignedRequest {
  std::string canonical_request;  // exposed for tests (sha256 compared to AWS vectors)
  std::string string_to_sign;
  std::string signature;          // lowercase hex
  std::string signed_headers;
  std::string authorization;      // full Authorization header value
};
SignedRequest sign_request(const Credentials& creds, const RequestToSign& req);

// A presigned GET/HEAD/PUT/DELETE URL (query auth, SignedHeaders=host, UNSIGNED-PAYLOAD).
struct PresignInput {
  std::string method = "GET";
  std::string scheme = "https";  // "http" only for the mock (cfg.allow_insecure_http)
  std::string host;              // "<account>.r2.cloudflarestorage.com" (+ ":port" when non-default)
  std::string raw_path;          // unencoded "/bucket/key"
  KeyValues extra_query;         // e.g. {"response-content-type", …}, {"response-content-disposition", …}
  std::string amz_date;
  int64_t expires_sec = 300;     // 1 … 604800 (R2/AWS limit); out of range → std::invalid_argument
  std::string region = "auto";
  std::string service = "s3";
};

struct PresignedUrl {
  std::string url;                // scheme://host<canonical_uri>?<canonical query incl. X-Amz-Signature>
  std::string canonical_request;  // for tests
  std::string signature;          // lowercase hex
};
PresignedUrl presign(const Credentials& creds, const PresignInput& in);

}  // namespace azm::storage::sigv4
