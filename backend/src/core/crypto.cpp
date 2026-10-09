#include "core/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <fstream>
#include <limits>

namespace azm::crypto {
namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kHex[] = "0123456789abcdef";

std::string b64_encode_with(std::string_view in, const char* alphabet, bool pad) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  const auto* d = reinterpret_cast<const unsigned char*>(in.data());
  std::size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const uint32_t v = (uint32_t{d[i]} << 16) | (uint32_t{d[i + 1]} << 8) | d[i + 2];
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
    out.push_back(alphabet[(v >> 6) & 63]);
    out.push_back(alphabet[v & 63]);
  }
  const std::size_t rest = in.size() - i;
  if (rest == 1) {
    const uint32_t v = uint32_t{d[i]} << 16;
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
    if (pad) out.append("==");
  } else if (rest == 2) {
    const uint32_t v = (uint32_t{d[i]} << 16) | (uint32_t{d[i + 1]} << 8);
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
    out.push_back(alphabet[(v >> 6) & 63]);
    if (pad) out.push_back('=');
  }
  return out;
}

int b64_value(char c, bool url) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (!url && c == '+') return 62;
  if (!url && c == '/') return 63;
  if (url && c == '-') return 62;
  if (url && c == '_') return 63;
  return -1;
}

bool is_b64_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::optional<std::string> b64_decode_with(std::string_view in, bool url, bool allow_space) {
  std::string out;
  out.reserve(in.size() / 4 * 3 + 3);
  uint32_t acc = 0;
  int bits = 0;
  std::size_t data_chars = 0, pad_chars = 0;
  for (char c : in) {
    if (allow_space && is_b64_space(c)) continue;
    if (c == '=') {
      ++pad_chars;
      continue;
    }
    if (pad_chars > 0) return std::nullopt;  // data after padding
    const int v = b64_value(c, url);
    if (v < 0) return std::nullopt;
    ++data_chars;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xff));
    }
  }
  const std::size_t rem = data_chars % 4;
  if (rem == 1) return std::nullopt;  // impossible length
  if (pad_chars > 0) {
    // Padding, if present, must complete the final quantum exactly.
    if (pad_chars > 2 || (data_chars + pad_chars) % 4 != 0) return std::nullopt;
  }
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};

}  // namespace

// ---- randomness ----------------------------------------------------------------------------

std::string random_bytes(std::size_t n) {
  std::string out(n, '\0');
  if (n == 0) return out;
  if (n > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw CryptoError("random_bytes: request too large");
  if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) != 1)
    throw CryptoError("RAND_bytes failed");
  return out;
}

std::string random_token_b64url(std::size_t nbytes) { return b64url_encode(random_bytes(nbytes)); }

std::string uuid_v4() {
  std::string b = random_bytes(16);
  b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0f) | 0x40);  // version 4
  b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3f) | 0x80);  // RFC 4122 variant
  const std::string h = hex_encode(b);
  std::string out;
  out.reserve(36);
  out.append(h, 0, 8).push_back('-');
  out.append(h, 8, 4).push_back('-');
  out.append(h, 12, 4).push_back('-');
  out.append(h, 16, 4).push_back('-');
  out.append(h, 20, 12);
  return out;
}

// ---- encodings -----------------------------------------------------------------------------

std::string b64_encode(std::string_view data) { return b64_encode_with(data, kB64, true); }
std::optional<std::string> b64_decode(std::string_view text) {
  return b64_decode_with(text, false, true);
}
std::string b64url_encode(std::string_view data) { return b64_encode_with(data, kB64Url, false); }
std::optional<std::string> b64url_decode(std::string_view text) {
  return b64_decode_with(text, true, false);
}

std::string hex_encode(std::string_view data) {
  std::string out;
  out.reserve(data.size() * 2);
  for (unsigned char c : data) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xf]);
  }
  return out;
}

std::optional<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_value(hex[i]), lo = hex_value(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

// ---- hashing / MAC -------------------------------------------------------------------------

struct Sha256::Ctx {
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> md;
  bool finished = false;
};

Sha256::Sha256() : ctx_(std::make_unique<Ctx>()) {
  ctx_->md.reset(EVP_MD_CTX_new());
  if (!ctx_->md || EVP_DigestInit_ex(ctx_->md.get(), EVP_sha256(), nullptr) != 1)
    throw CryptoError("EVP_DigestInit_ex(sha256) failed");
}
Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(const void* data, std::size_t len) {
  if (!ctx_ || ctx_->finished) throw CryptoError("Sha256::update after final");
  if (len == 0) return;
  if (EVP_DigestUpdate(ctx_->md.get(), data, len) != 1) throw CryptoError("EVP_DigestUpdate failed");
  bytes_ += len;
}

std::string Sha256::final_raw() {
  if (!ctx_ || ctx_->finished) throw CryptoError("Sha256::final called twice");
  std::string out(32, '\0');
  unsigned int len = 0;
  if (EVP_DigestFinal_ex(ctx_->md.get(), reinterpret_cast<unsigned char*>(out.data()), &len) != 1 ||
      len != 32)
    throw CryptoError("EVP_DigestFinal_ex failed");
  ctx_->finished = true;
  return out;
}

std::string Sha256::final_hex() { return hex_encode(final_raw()); }

std::string sha256(std::string_view data) {
  std::string out(32, '\0');
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), reinterpret_cast<unsigned char*>(out.data()), &len,
                 EVP_sha256(), nullptr) != 1 ||
      len != 32)
    throw CryptoError("EVP_Digest(sha256) failed");
  return out;
}

std::string sha256_hex(std::string_view data) { return hex_encode(sha256(data)); }

std::string sha256_file_hex(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("sha256_file_hex: cannot open " + file.string());
  Sha256 h;
  std::array<char, 64 * 1024> buf;
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = in.gcount();
    if (n > 0) h.update(buf.data(), static_cast<std::size_t>(n));
  }
  if (in.bad()) throw std::runtime_error("sha256_file_hex: read error on " + file.string());
  return h.final_hex();
}

std::string hmac_sha256(std::string_view key, std::string_view data) {
  std::string out(32, '\0');
  std::size_t len = 0;
  // EVP_Q_mac rejects a NULL key pointer even when keylen == 0.
  static const unsigned char kEmpty[1] = {0};
  const unsigned char* k =
      key.empty() ? kEmpty : reinterpret_cast<const unsigned char*>(key.data());
  if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, k, key.size(),
                reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                reinterpret_cast<unsigned char*>(out.data()), out.size(), &len) == nullptr ||
      len != 32)
    throw CryptoError("EVP_Q_mac(HMAC-SHA256) failed");
  return out;
}

bool ct_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ---- scrypt ----------------------------------------------------------------------------------
namespace {

constexpr uint64_t kMinMaxmem = 64ull << 20;

bool params_in_range(int ln, int r, int p) {
  // Upper bounds protect against hostile/corrupt encodings (CPU / memory DoS).
  return ln >= 1 && ln <= 20 && r >= 1 && r <= 32 && p >= 1 && p <= 16;
}

std::optional<std::string> scrypt_raw(std::string_view pw, std::string_view salt, int ln, int r,
                                      int p, std::size_t key_len) {
  const uint64_t N = uint64_t{1} << ln;
  // Memory needed by OpenSSL: 128*r*(N + p + 2) bytes, plus slack.
  const uint64_t need = 128ull * static_cast<uint64_t>(r) * (N + static_cast<uint64_t>(p) + 2);
  // Both operands must be uint64_t: on Linux LP64 uint64_t is `unsigned long`, so a `1ull`
  // literal here would deduce conflicting types for std::max (GCC/libstdc++ hard error).
  const uint64_t maxmem = std::max(kMinMaxmem, need + (uint64_t{1} << 20));
  std::string out(key_len, '\0');
  if (EVP_PBE_scrypt(pw.data(), pw.size(), reinterpret_cast<const unsigned char*>(salt.data()),
                     salt.size(), N, static_cast<uint64_t>(r), static_cast<uint64_t>(p), maxmem,
                     reinterpret_cast<unsigned char*>(out.data()), out.size()) != 1)
    return std::nullopt;
  return out;
}

bool parse_int(std::string_view s, int& out) {
  if (s.empty()) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

}  // namespace

std::string password_hash(std::string_view password, const ScryptParams& prm) {
  if (!params_in_range(prm.ln, prm.r, prm.p) || prm.salt_len < 8 || prm.key_len < 16 ||
      prm.key_len > 128)
    throw std::invalid_argument("password_hash: scrypt parameters out of range");
  const std::string salt = random_bytes(prm.salt_len);
  auto key = scrypt_raw(password, salt, prm.ln, prm.r, prm.p, prm.key_len);
  if (!key) throw CryptoError("EVP_PBE_scrypt failed");
  std::string out = "$scrypt$ln=" + std::to_string(prm.ln) + ",r=" + std::to_string(prm.r) +
                    ",p=" + std::to_string(prm.p) + "$";
  out += b64_encode(salt);
  out += '$';
  out += b64_encode(*key);
  return out;
}

bool password_verify(std::string_view password, std::string_view encoded) {
  constexpr std::string_view kPrefix = "$scrypt$";
  if (!encoded.starts_with(kPrefix)) return false;
  std::string_view rest = encoded.substr(kPrefix.size());
  const auto d1 = rest.find('$');
  if (d1 == std::string_view::npos) return false;
  const std::string_view params = rest.substr(0, d1);
  rest = rest.substr(d1 + 1);
  const auto d2 = rest.find('$');
  if (d2 == std::string_view::npos) return false;
  const std::string_view salt_b64 = rest.substr(0, d2);
  const std::string_view hash_b64 = rest.substr(d2 + 1);

  int ln = -1, r = -1, p = -1;
  std::size_t pos = 0;
  while (pos <= params.size()) {
    auto comma = params.find(',', pos);
    if (comma == std::string_view::npos) comma = params.size();
    const std::string_view kv = params.substr(pos, comma - pos);
    const auto eq = kv.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
    int val = 0;
    if (!parse_int(v, val)) return false;
    if (k == "ln") ln = val;
    else if (k == "r") r = val;
    else if (k == "p") p = val;
    else return false;
    pos = comma + 1;
  }
  if (!params_in_range(ln, r, p)) return false;

  const auto salt = b64_decode(salt_b64);
  const auto expected = b64_decode(hash_b64);
  if (!salt || !expected || salt->empty() || expected->size() < 16 || expected->size() > 128)
    return false;
  const auto actual = scrypt_raw(password, *salt, ln, r, p, expected->size());
  if (!actual) return false;
  return ct_equal(*actual, *expected);
}

}  // namespace azm::crypto
