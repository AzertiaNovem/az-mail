#include "core/crypto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <set>

using namespace azm::crypto;

TEST_CASE("sha256 known answers", "[crypto]") {
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256("abc").size() == 32);
  CHECK(hex_encode(sha256("abc")) == sha256_hex("abc"));
}

TEST_CASE("Sha256 incremental matches one-shot", "[crypto]") {
  Sha256 h;
  h.update("a");
  h.update(std::string_view("bc"));
  h.update(nullptr, 0);
  CHECK(h.bytes() == 3);
  CHECK(h.final_hex() == sha256_hex("abc"));
  CHECK_THROWS_AS(h.update("x"), CryptoError);

  // Large streamed input equals one-shot.
  std::string big(1 << 20, 'z');
  Sha256 h2;
  for (std::size_t i = 0; i < big.size(); i += 4096) h2.update(std::string_view(big).substr(i, 4096));
  CHECK(h2.final_hex() == sha256_hex(big));

  Sha256 moved = Sha256();
  moved.update("abc");
  CHECK(moved.final_raw() == sha256("abc"));
}

TEST_CASE("HMAC-SHA256 RFC 4231 vectors", "[crypto]") {
  // Test case 1
  CHECK(hex_encode(hmac_sha256(std::string(20, '\x0b'), "Hi There")) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  // Test case 2
  CHECK(hex_encode(hmac_sha256("Jefe", "what do ya want for nothing?")) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // Empty key and message
  CHECK(hex_encode(hmac_sha256("", "")) ==
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

TEST_CASE("base64 RFC 4648 vectors and roundtrip", "[crypto]") {
  const std::pair<std::string, std::string> vectors[] = {
      {"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},     {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
  for (const auto& [plain, enc] : vectors) {
    CHECK(b64_encode(plain) == enc);
    REQUIRE(b64_decode(enc).has_value());
    CHECK(*b64_decode(enc) == plain);
  }
  // Binary roundtrip for every length 0..64.
  for (std::size_t n = 0; n <= 64; ++n) {
    const std::string bytes = random_bytes(n);
    CHECK(b64_decode(b64_encode(bytes)).value() == bytes);
    CHECK(b64url_decode(b64url_encode(bytes)).value() == bytes);
    CHECK(hex_decode(hex_encode(bytes)).value() == bytes);
  }
}

TEST_CASE("base64 decode tolerance and invalid input", "[crypto]") {
  CHECK(b64_decode("Zm9v\r\nYmFy").value() == "foobar");  // MIME line breaks
  CHECK(b64_decode(" Zm9v YmE= ").value() == "fooba");
  CHECK(b64_decode("Zg").value() == "f");  // missing padding is accepted
  CHECK_FALSE(b64_decode("Zm9v!").has_value());
  CHECK_FALSE(b64_decode("Z").has_value());       // impossible length
  CHECK_FALSE(b64_decode("Zg=a").has_value());    // data after padding
  CHECK_FALSE(b64_decode("Zg===").has_value());   // too much padding
  CHECK_FALSE(b64_decode("Zg=").has_value());     // partial padding
  CHECK_FALSE(b64_decode("-_8").has_value());     // URL alphabet is not standard
}

TEST_CASE("base64url alphabet without padding", "[crypto]") {
  const std::string bytes = "\xfb\xff";
  CHECK(b64_encode(bytes) == "+/8=");
  CHECK(b64url_encode(bytes) == "-_8");
  CHECK(b64url_decode("-_8").value() == bytes);
  CHECK(b64url_decode("-_8=").value() == bytes);  // padding optional
  CHECK_FALSE(b64url_decode("+/8").has_value());
  CHECK_FALSE(b64url_decode("-_8\n").has_value());  // no whitespace in URL tokens
  CHECK(random_token_b64url(32).size() == 43);
}

TEST_CASE("hex encode/decode", "[crypto]") {
  CHECK(hex_encode(std::string("\x01\xab\xff", 3)) == "01abff");
  CHECK(hex_decode("01ABff").value() == std::string("\x01\xab\xff", 3));
  CHECK_FALSE(hex_decode("abc").has_value());
  CHECK_FALSE(hex_decode("0g").has_value());
  const std::vector<uint8_t> v = to_bytes("hi");
  CHECK(hex_encode(std::span<const uint8_t>(v)) == "6869");
}

TEST_CASE("ct_equal", "[crypto]") {
  CHECK(ct_equal("abc", "abc"));
  CHECK(ct_equal("", ""));
  CHECK_FALSE(ct_equal("abc", "abd"));
  CHECK_FALSE(ct_equal("abc", "abcd"));
}

TEST_CASE("uuid_v4 format and uniqueness", "[crypto]") {
  const std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  std::set<std::string> seen;
  for (int i = 0; i < 200; ++i) {
    const std::string u = uuid_v4();
    CHECK(std::regex_match(u, re));
    seen.insert(u);
  }
  CHECK(seen.size() == 200);
}

TEST_CASE("random_bytes", "[crypto]") {
  CHECK(random_bytes(0).empty());
  CHECK(random_bytes(17).size() == 17);
  CHECK(random_bytes(32) != random_bytes(32));
}

TEST_CASE("scrypt password hash roundtrip (ln=10)", "[crypto][scrypt]") {
  ScryptParams p;
  p.ln = 10;
  const std::string enc = password_hash("correct horse 电池", p);
  CHECK(enc.starts_with("$scrypt$ln=10,r=8,p=1$"));
  CHECK(password_verify("correct horse 电池", enc));
  CHECK_FALSE(password_verify("correct horse", enc));
  CHECK_FALSE(password_verify("", enc));
  // Fresh salt each time.
  CHECK(password_hash("pw", p) != password_hash("pw", p));
}

TEST_CASE("scrypt default parameters (N=2^15, r=8) work despite OpenSSL maxmem default",
          "[crypto][scrypt]") {
  const std::string enc = password_hash("hunter2");
  CHECK(enc.starts_with("$scrypt$ln=15,r=8,p=1$"));
  CHECK(password_verify("hunter2", enc));
  CHECK_FALSE(password_verify("hunter3", enc));
}

TEST_CASE("scrypt RFC 7914 known answer via encoded form", "[crypto][scrypt]") {
  // scrypt(P="password", S="NaCl", N=1024, r=8, p=16, dkLen=64)
  const std::string enc =
      "$scrypt$ln=10,r=8,p=16$TmFDbA==$"
      "/bq+HJ00cgB4VucZDQHp/nxq18vII3gw53N2Y0s3MWIurzDZLiKjiG/xCSedmDDaxyevuUqD7m2DYMvfoswGQA==";
  CHECK(password_verify("password", enc));
  CHECK_FALSE(password_verify("Password", enc));
}

TEST_CASE("scrypt verify rejects malformed encodings", "[crypto][scrypt]") {
  CHECK_FALSE(password_verify("pw", ""));
  CHECK_FALSE(password_verify("pw", "$argon2id$v=19$m=65536$abc$def"));
  CHECK_FALSE(password_verify("pw", "$scrypt$ln=10,r=8,p=1$c2FsdA=="));
  CHECK_FALSE(password_verify("pw", "$scrypt$ln=10,r=8$c2FsdHNhbHQ=$aGFzaGhhc2hoYXNoaGFzaA=="));
  CHECK_FALSE(password_verify("pw", "$scrypt$ln=99,r=8,p=1$c2FsdHNhbHQ=$aGFzaGhhc2hoYXNoaGFzaA=="));
  CHECK_FALSE(password_verify("pw", "$scrypt$ln=10,r=8,p=1$!!!$aGFzaGhhc2hoYXNoaGFzaA=="));
  CHECK_FALSE(password_verify("pw", "$scrypt$ln=x,r=8,p=1$c2FsdHNhbHQ=$aGFzaGhhc2hoYXNoaGFzaA=="));
  ScryptParams bad;
  bad.ln = 0;
  CHECK_THROWS_AS(password_hash("pw", bad), std::invalid_argument);
}
