// Owner: WP-C — Svix webhook signature verification (test vector from the brief / DESIGN §6).
#include "core/crypto.hpp"
#include "resend/svix.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace azm;
using namespace azm::resend;

namespace {
constexpr std::string_view kSecret = "whsec_plJ3nmyCDGBKInavdOK15jsl";
constexpr std::string_view kId = "msg_loFOjxBNrRLzqYUf";
constexpr std::string_view kTs = "1731705121";
constexpr std::string_view kBody = R"({"event_type":"ping","data":{"success":true}})";
constexpr std::string_view kSig = "v1,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0=";
constexpr int64_t kNow = 1731705121;
}  // namespace

TEST_CASE("svix: published test vector signs and verifies", "[svix]") {
  CHECK(sign_svix(kSecret, kId, kTs, kBody) == kSig);
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow) == SvixResult::Ok);
  // Secret without the whsec_ prefix is accepted too.
  CHECK(verify_svix("plJ3nmyCDGBKInavdOK15jsl", kId, kTs, kSig, kBody, kNow) == SvixResult::Ok);
}

TEST_CASE("svix: multiple signatures, any v1 match passes", "[svix]") {
  const std::string other = "v1," + crypto::b64_encode(std::string(32, 'x'));
  CHECK(verify_svix(kSecret, kId, kTs, other + " " + std::string(kSig), kBody, kNow) == SvixResult::Ok);
  CHECK(verify_svix(kSecret, kId, kTs, std::string(kSig) + "  " + other, kBody, kNow) == SvixResult::Ok);
  // Unknown versions are ignored; a v1a entry with the right bytes does not count.
  CHECK(verify_svix(kSecret, kId, kTs, "v1a,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0=", kBody, kNow) ==
        SvixResult::BadSignature);
  CHECK(verify_svix(kSecret, kId, kTs, other, kBody, kNow) == SvixResult::BadSignature);
  CHECK(verify_svix(kSecret, kId, kTs, "garbage", kBody, kNow) == SvixResult::BadSignature);
}

TEST_CASE("svix: tampering is detected", "[svix]") {
  CHECK(verify_svix(kSecret, kId, kTs, kSig, std::string(kBody) + " ", kNow) == SvixResult::BadSignature);
  CHECK(verify_svix(kSecret, "msg_other", kTs, kSig, kBody, kNow) == SvixResult::BadSignature);
  CHECK(verify_svix("whsec_" + crypto::b64_encode("another secret"), kId, kTs, kSig, kBody, kNow) ==
        SvixResult::BadSignature);
  // A changed timestamp inside the tolerance still fails the signature.
  CHECK(verify_svix(kSecret, kId, "1731705122", kSig, kBody, kNow) == SvixResult::BadSignature);
}

TEST_CASE("svix: timestamp tolerance (300 s default)", "[svix]") {
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow + 300) == SvixResult::Ok);
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow - 300) == SvixResult::Ok);
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow + 301) == SvixResult::BadTimestamp);
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow - 360) == SvixResult::BadTimestamp);
  CHECK(verify_svix(kSecret, kId, kTs, kSig, kBody, kNow + 1000, 2000) == SvixResult::Ok);
  CHECK(verify_svix(kSecret, kId, "17317x5121", kSig, kBody, kNow) == SvixResult::BadTimestamp);
  CHECK(verify_svix(kSecret, kId, "-5", kSig, kBody, kNow) == SvixResult::BadTimestamp);
  CHECK(verify_svix(kSecret, kId, "99999999999999999999999", kSig, kBody, kNow) == SvixResult::BadTimestamp);
}

TEST_CASE("svix: missing headers and malformed secrets", "[svix]") {
  CHECK(verify_svix(kSecret, "", kTs, kSig, kBody, kNow) == SvixResult::MissingHeaders);
  CHECK(verify_svix(kSecret, kId, "", kSig, kBody, kNow) == SvixResult::MissingHeaders);
  CHECK(verify_svix(kSecret, kId, kTs, "  ", kBody, kNow) == SvixResult::MissingHeaders);
  CHECK(verify_svix("", kId, kTs, kSig, kBody, kNow) == SvixResult::BadSignature);
  CHECK(verify_svix("whsec_", kId, kTs, kSig, kBody, kNow) == SvixResult::BadSignature);
  CHECK(verify_svix("whsec_!!!not-base64", kId, kTs, kSig, kBody, kNow) == SvixResult::BadSignature);
  CHECK_THROWS_AS(sign_svix("whsec_", kId, kTs, kBody), std::invalid_argument);
  CHECK(to_string(SvixResult::Ok) == "ok");
  CHECK(to_string(SvixResult::MissingHeaders) == "missing_headers");
  CHECK(to_string(SvixResult::BadSignature) == "bad_signature");
}
