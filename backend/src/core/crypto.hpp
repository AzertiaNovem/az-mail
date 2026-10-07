// Cryptographic helpers over OpenSSL 3 (EVP APIs only).
// Convention: binary data is carried in std::string (bytes); use to_bytes() for SQLite BLOBs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace azm::crypto {

// Thrown when OpenSSL itself fails (RNG, digest init, ...). Never thrown for bad user input.
struct CryptoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline std::string_view as_view(std::span<const uint8_t> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}
inline std::vector<uint8_t> to_bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()),
          reinterpret_cast<const uint8_t*>(s.data()) + s.size()};
}

// ---- randomness ----------------------------------------------------------------------------
std::string random_bytes(std::size_t n);                  // CSPRNG (RAND_bytes)
std::string random_token_b64url(std::size_t nbytes = 32);  // e.g. session tokens
std::string uuid_v4();                                     // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"

// ---- encodings -----------------------------------------------------------------------------
std::string b64_encode(std::string_view data);  // standard alphabet, with '=' padding
inline std::string b64_encode(std::span<const uint8_t> d) { return b64_encode(as_view(d)); }
// Standard alphabet; ASCII whitespace ignored; padding optional. nullopt on invalid input.
std::optional<std::string> b64_decode(std::string_view text);

std::string b64url_encode(std::string_view data);  // URL-safe alphabet, no padding
inline std::string b64url_encode(std::span<const uint8_t> d) { return b64url_encode(as_view(d)); }
std::optional<std::string> b64url_decode(std::string_view text);  // padding optional

std::string hex_encode(std::string_view data);  // lowercase
inline std::string hex_encode(std::span<const uint8_t> d) { return hex_encode(as_view(d)); }
std::optional<std::string> hex_decode(std::string_view hex);  // case-insensitive, even length

// ---- hashing / MAC -------------------------------------------------------------------------
std::string sha256(std::string_view data);      // 32 raw bytes
std::string sha256_hex(std::string_view data);  // 64 lowercase hex chars
std::string sha256_file_hex(const std::filesystem::path& file);  // streams; throws on I/O error
std::string hmac_sha256(std::string_view key, std::string_view data);  // 32 raw bytes

// Incremental SHA-256 for streaming (uploads, downloads). Not copyable; movable.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(Sha256&&) noexcept;
  Sha256& operator=(Sha256&&) noexcept;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(const void* data, std::size_t len);
  void update(std::string_view data) { update(data.data(), data.size()); }
  std::string final_raw();  // 32 raw bytes; the object must not be updated afterwards
  std::string final_hex();  // 64 lowercase hex chars
  std::uint64_t bytes() const { return bytes_; }

 private:
  struct Ctx;
  std::unique_ptr<Ctx> ctx_;
  std::uint64_t bytes_ = 0;
};

// Constant-time equality (lengths compared first; length is not secret).
bool ct_equal(std::string_view a, std::string_view b);

// ---- password hashing (scrypt) ---------------------------------------------------------------
// Encoded form: "$scrypt$ln=15,r=8,p=1$<b64 salt>$<b64 hash>".
struct ScryptParams {
  int ln = 15;  // N = 2^ln
  int r = 8;
  int p = 1;
  std::size_t salt_len = 16;
  std::size_t key_len = 32;
};
// NOTE: N=2^15,r=8 needs ~32 MiB which exceeds OpenSSL's default maxmem (32 MB): we always pass
// an explicit maxmem (>= 64 MiB) to EVP_PBE_scrypt.
std::string password_hash(std::string_view password, const ScryptParams& params = {});
// Constant-time verify; false for malformed encodings or out-of-range parameters.
bool password_verify(std::string_view password, std::string_view encoded);

}  // namespace azm::crypto
