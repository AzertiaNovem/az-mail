// Owner: WP-C
//
// Svix webhook signature verification (Resend webhooks; DESIGN §3, E2E 19).
// Signed content = "<svix-id>.<svix-timestamp>.<raw body>", HMAC-SHA256 with the base64 secret
// after the "whsec_" prefix; the header holds space-separated "v1,<base64 sig>" entries (any
// match passes; constant-time compare). The timestamp must be within ±tolerance_s of now_s.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace azm::resend {

enum class SvixResult { Ok, MissingHeaders, BadTimestamp, BadSignature };

// Headers: svix-id (`id`), svix-timestamp (`ts`, decimal seconds), svix-signature (`sig_header`).
// MissingHeaders when any is empty; BadTimestamp when `ts` is not an integer or is outside the
// tolerance; BadSignature when the secret is malformed or no v1 signature matches.
SvixResult verify_svix(std::string_view whsec, std::string_view id, std::string_view ts,
                       std::string_view sig_header, std::string_view body, int64_t now_s,
                       int tolerance_s = 300);

// "v1,<base64(HMAC)>" for the given inputs (used by tests and the mock-parity self-test).
// Throws std::invalid_argument for a malformed secret.
std::string sign_svix(std::string_view whsec, std::string_view id, std::string_view ts,
                      std::string_view body);

std::string_view to_string(SvixResult r);  // "ok", "missing_headers", "bad_timestamp", "bad_signature"

}  // namespace azm::resend
