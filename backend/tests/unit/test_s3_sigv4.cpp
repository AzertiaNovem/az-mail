// Owner: WP-C — AWS SigV4 signing, verified byte-exact against the AWS S3 documentation
// vectors listed in DESIGN Addendum A.2.
#include "core/crypto.hpp"
#include "core/time.hpp"
#include "storage/s3_sigv4.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace azm;
using namespace azm::storage;

namespace {

const sigv4::Credentials kCreds{"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"};
constexpr std::string_view kDate = "20130524T000000Z";
constexpr std::string_view kHost = "examplebucket.s3.amazonaws.com";

sigv4::RequestToSign aws_request(std::string method, std::string path, sigv4::KeyValues query,
                                 sigv4::KeyValues extra_headers, std::string payload_sha) {
  sigv4::RequestToSign r;
  r.method = std::move(method);
  r.raw_path = std::move(path);
  r.query = std::move(query);
  r.headers = {{"host", std::string(kHost)},
               {"x-amz-content-sha256", payload_sha},
               {"x-amz-date", std::string(kDate)}};
  for (auto& h : extra_headers) r.headers.push_back(std::move(h));
  r.payload_sha256 = std::move(payload_sha);
  r.amz_date = std::string(kDate);
  r.region = "us-east-1";
  r.service = "s3";
  return r;
}

}  // namespace

TEST_CASE("sigv4: signing key vector", "[sigv4]") {
  const std::string key = sigv4::signing_key(kCreds.secret_access_key, "20130524", "us-east-1", "s3");
  CHECK(crypto::hex_encode(key) == "dbb893acc010964918f1fd433add87c70e8b0db6be30c1fbeafefa5ec6ba8378");
}

TEST_CASE("sigv4: GET object with Range (AWS vector)", "[sigv4]") {
  auto req = aws_request("GET", "/test.txt", {}, {{"Range", "bytes=0-9"}},
                         std::string(sigv4::kEmptyPayloadSha256));
  const auto s = sigv4::sign_request(kCreds, req);
  CHECK(s.canonical_request ==
        "GET\n/test.txt\n\nhost:examplebucket.s3.amazonaws.com\nrange:bytes=0-9\n"
        "x-amz-content-sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
        "x-amz-date:20130524T000000Z\n\nhost;range;x-amz-content-sha256;x-amz-date\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(crypto::sha256_hex(s.canonical_request) ==
        "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
  CHECK(s.string_to_sign ==
        "AWS4-HMAC-SHA256\n20130524T000000Z\n20130524/us-east-1/s3/aws4_request\n"
        "7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972");
  CHECK(s.signed_headers == "host;range;x-amz-content-sha256;x-amz-date");
  CHECK(s.signature == "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
  CHECK(s.authorization ==
        "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
        "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
        "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST_CASE("sigv4: PUT object with $ in the key (AWS vector)", "[sigv4]") {
  const std::string body = "Welcome to Amazon S3.";
  const std::string sha = crypto::sha256_hex(body);
  REQUIRE(sha == "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
  auto req = aws_request("PUT", "/test$file.text", {},
                         {{"Date", "Fri, 24 May 2013 00:00:00 GMT"},
                          {"x-amz-storage-class", "REDUCED_REDUNDANCY"}},
                         sha);
  const auto s = sigv4::sign_request(kCreds, req);
  CHECK(s.canonical_request.rfind("PUT\n/test%24file.text\n\n", 0) == 0);
  CHECK(crypto::sha256_hex(s.canonical_request) ==
        "9e0e90d9c76de8fa5b200d8c849cd5b8dc7a3be3951ddb7f6a76b4158342019d");
  CHECK(s.signed_headers == "date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class");
  CHECK(s.signature == "98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd");
}

TEST_CASE("sigv4: GET bucket lifecycle (AWS vector)", "[sigv4]") {
  auto req = aws_request("GET", "/", {{"lifecycle", ""}}, {}, std::string(sigv4::kEmptyPayloadSha256));
  const auto s = sigv4::sign_request(kCreds, req);
  CHECK(s.canonical_request.rfind("GET\n/\nlifecycle=\n", 0) == 0);
  CHECK(s.signed_headers == "host;x-amz-content-sha256;x-amz-date");
  CHECK(s.signature == "fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543");
}

TEST_CASE("sigv4: GET bucket list objects (AWS vector)", "[sigv4]") {
  // Deliberately unsorted input: the canonical query sorts after encoding.
  auto req = aws_request("GET", "/", {{"prefix", "J"}, {"max-keys", "2"}}, {},
                         std::string(sigv4::kEmptyPayloadSha256));
  const auto s = sigv4::sign_request(kCreds, req);
  CHECK(s.canonical_request.rfind("GET\n/\nmax-keys=2&prefix=J\n", 0) == 0);
  CHECK(s.signature == "34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7");
}

TEST_CASE("sigv4: presigned GET (AWS vector)", "[sigv4]") {
  sigv4::PresignInput in;
  in.method = "GET";
  in.scheme = "https";
  in.host = std::string(kHost);
  in.raw_path = "/test.txt";
  in.amz_date = std::string(kDate);
  in.expires_sec = 86400;
  in.region = "us-east-1";
  in.service = "s3";
  const auto p = sigv4::presign(kCreds, in);
  CHECK(p.canonical_request ==
        "GET\n/test.txt\n"
        "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
        "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host\n"
        "host:examplebucket.s3.amazonaws.com\n\nhost\nUNSIGNED-PAYLOAD");
  CHECK(crypto::sha256_hex(p.canonical_request) ==
        "3bfa292879f6447bbcda7001decf97f4a54dc650c8942174ae0a9121cf58ad04");
  CHECK(p.signature == "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
  CHECK(p.url ==
        "https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
        "&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host"
        "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
}

TEST_CASE("sigv4: presign with response overrides sorts and encodes them", "[sigv4]") {
  sigv4::PresignInput in;
  in.host = "acct.r2.cloudflarestorage.com";
  in.raw_path = "/bucket/azmail/blobs/ab/cd/x";
  in.amz_date = "20261007T120000Z";
  in.extra_query = {{"response-content-type", "text/plain; charset=utf-8"},
                    {"response-content-disposition", "attachment; filename*=UTF-8''%E6%8A%A5.txt"}};
  const auto p = sigv4::presign(kCreds, in);
  // Region "auto" (R2) is part of the scope; '/' and ';' etc. are percent-encoded in values;
  // the '%' of an already-encoded filename* is encoded again (it is a raw value here).
  CHECK(p.url.find("X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20261007%2Fauto%2Fs3%2Faws4_request") !=
        std::string::npos);
  CHECK(p.url.find("response-content-type=text%2Fplain%3B%20charset%3Dutf-8") != std::string::npos);
  CHECK(p.url.find("response-content-disposition=attachment%3B%20filename%2A%3DUTF-8%27%27%25E6%258A%25A5.txt") !=
        std::string::npos);
  // X-Amz-* sort before response-* (uppercase < lowercase in byte order).
  CHECK(p.url.find("X-Amz-SignedHeaders=host&response-content-disposition=") != std::string::npos);
  CHECK(p.url.rfind("https://acct.r2.cloudflarestorage.com/bucket/azmail/blobs/ab/cd/x?", 0) == 0);
}

TEST_CASE("sigv4: encoding helpers", "[sigv4]") {
  CHECK(sigv4::uri_encode("a b/c~d-e.f_g") == "a%20b%2Fc~d-e.f_g");
  CHECK(sigv4::uri_encode("a b/c", false) == "a%20b/c");
  CHECK(sigv4::uri_encode("\xe4\xb8\xad") == "%E4%B8%AD");  // uppercase hex, per byte
  CHECK(sigv4::uri_encode("+*=&") == "%2B%2A%3D%26");
  CHECK(sigv4::canonical_uri("") == "/");
  CHECK(sigv4::canonical_uri("/bucket/a b+c") == "/bucket/a%20b%2Bc");
  CHECK(sigv4::canonical_uri("/a//b/../c") == "/a//b/../c");  // never normalized
  CHECK(sigv4::canonical_query({}) == "");
  const sigv4::KeyValues q{{"b", "2"}, {"a", "x/y"}, {"a", "1"}, {"c", ""}};
  CHECK(sigv4::canonical_query(q) == "a=1&a=x%2Fy&b=2&c=");
}

TEST_CASE("sigv4: canonical headers trim, collapse, lowercase and merge", "[sigv4]") {
  const sigv4::KeyValues h{{"X-Amz-Meta-B", "  two   words  "}, {"Host", "h"}, {"x-amz-meta-b", "x"}};
  const auto ch = sigv4::canonical_headers(h);
  CHECK(ch.block == "host:h\nx-amz-meta-b:two words,x\n");
  CHECK(ch.signed_headers == "host;x-amz-meta-b");
}

TEST_CASE("sigv4: amz_date and argument validation", "[sigv4]") {
  CHECK(sigv4::amz_date(0) == "19700101T000000Z");
  CHECK(sigv4::amz_date(*utc_ms(2013, 5, 24, 0, 0, 0, 999)) == "20130524T000000Z");
  CHECK(sigv4::amz_date(*utc_ms(2026, 12, 31, 23, 59, 58)) == "20261231T235958Z");
  CHECK(sigv4::credential_scope("20130524", "auto", "s3") == "20130524/auto/s3/aws4_request");

  sigv4::PresignInput in;
  in.host = "h";
  in.raw_path = "/b/k";
  in.amz_date = "20130524T000000Z";
  in.expires_sec = 0;
  CHECK_THROWS_AS(sigv4::presign(kCreds, in), std::invalid_argument);
  in.expires_sec = 604801;
  CHECK_THROWS_AS(sigv4::presign(kCreds, in), std::invalid_argument);
  in.expires_sec = 604800;
  CHECK_NOTHROW(sigv4::presign(kCreds, in));
  in.amz_date = "2013-05-24";
  CHECK_THROWS_AS(sigv4::presign(kCreds, in), std::invalid_argument);

  auto req = aws_request("GET", "/x", {}, {}, std::string(sigv4::kEmptyPayloadSha256));
  req.amz_date = "bad";
  CHECK_THROWS_AS(sigv4::sign_request(kCreds, req), std::invalid_argument);
}
