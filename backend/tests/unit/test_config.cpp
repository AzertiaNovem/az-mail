// Owner: WP-A — config loader: env-file parsing, precedence, value syntax, validation.
#include "app/config_loader.hpp"
#include "config.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace azm;
using app::EnvMap;

namespace {

bool any_contains(const std::vector<std::string>& v, std::string_view needle) {
  return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

// A Config that passes Serve validation.
Config valid_serve_config() {
  Config c;
  c.server_secret = std::string(test::kTestSecret);
  c.resend_api_key = "re_test_key";
  return c;
}

// Sets an environment variable for the scope of a test.
struct ScopedEnv {
  std::string name;
  ScopedEnv(std::string n, const char* value) : name(std::move(n)) { ::setenv(name.c_str(), value, 1); }
  ~ScopedEnv() { ::unsetenv(name.c_str()); }
};

}  // namespace

TEST_CASE("config: env file syntax", "[config]") {
  std::vector<std::string> errors;
  const auto m = app::parse_env_file(
      "# comment\n"
      "\n"
      "AZMAIL_PORT=9090\n"
      "  export AZMAIL_DATA_DIR = /var/lib/azmail  \n"
      "AZMAIL_SECRET=\"quoted # not a comment\"\n"
      "RESEND_API_KEY='re_single $NOEXPAND \\n'\n"
      "AZMAIL_CORS_ORIGINS=https://a.example, https://b.example # trailing comment\n"
      "AZMAIL_LOG_LEVEL=debug\r\n"
      "ESCAPED=\"line1\\nline2\\t\\\"q\\\"\"\n"
      "EMPTY=\n"
      "HASH=abc#def\n"
      "not a line\n"
      "1BAD=x\n"
      "UNTERMINATED=\"oops\n"
      "TRAILING=\"x\" junk\n",
      errors);
  CHECK(m.at("AZMAIL_PORT") == "9090");
  CHECK(m.at("AZMAIL_DATA_DIR") == "/var/lib/azmail");
  CHECK(m.at("AZMAIL_SECRET") == "quoted # not a comment");
  CHECK(m.at("RESEND_API_KEY") == "re_single $NOEXPAND \\n");
  CHECK(m.at("AZMAIL_CORS_ORIGINS") == "https://a.example, https://b.example");
  CHECK(m.at("AZMAIL_LOG_LEVEL") == "debug");
  CHECK(m.at("ESCAPED") == "line1\nline2\t\"q\"");
  CHECK(m.at("EMPTY").empty());
  CHECK(m.at("HASH") == "abc#def");
  CHECK(m.count("1BAD") == 0);
  CHECK(m.count("UNTERMINATED") == 0);
  CHECK(m.count("TRAILING") == 0);
  REQUIRE(errors.size() == 4);
  CHECK(errors[0].rfind("line 12:", 0) == 0);
  CHECK(errors[1].rfind("line 13:", 0) == 0);
  CHECK(errors[2].rfind("line 14:", 0) == 0);
  CHECK(errors[3].rfind("line 15:", 0) == 0);
}

TEST_CASE("config: read_env_file", "[config]") {
  test::TempDir td;
  const auto ok = td / "ok.env";
  std::ofstream(ok) << "AZMAIL_PORT=1234\n";
  CHECK(app::read_env_file(ok).at("AZMAIL_PORT") == "1234");
  const auto bad = td / "bad.env";
  std::ofstream(bad) << "garbage\n";
  CHECK_THROWS_AS(app::read_env_file(bad), std::runtime_error);
  CHECK_THROWS_AS(app::read_env_file(td / "missing.env"), std::runtime_error);
}

TEST_CASE("config: values are parsed into the Config", "[config]") {
  EnvMap env{{"AZMAIL_LISTEN_ADDRESS", "0.0.0.0"},
             {"AZMAIL_PORT", "8443"},
             {"AZMAIL_CORS_ORIGINS", " https://a.example , ,https://b.example "},
             {"AZMAIL_TRUSTED_PROXIES", "10.0.0.0/8,::1"},
             {"AZMAIL_MAX_INFLIGHT", "64"},
             {"AZMAIL_UPLOAD_LIMIT", "10M"},
             {"AZMAIL_JSON_BODY_LIMIT", "512K"},
             {"AZMAIL_DRAFT_BODY_LIMIT", "2MiB"},
             {"AZMAIL_INBOUND_RAW_LIMIT", "1G"},
             {"AZMAIL_UNROUTABLE_TO_ADMINS", "yes"},
             {"AZMAIL_ALLOW_INSECURE_HTTP", "1"},
             {"AZMAIL_SCHEDULE_MODE", "LOCAL"},
             {"AZMAIL_BLOB_BACKEND", "r2"},
             {"AZMAIL_FILES_DELIVERY", "redirect"},
             {"RESEND_RATE_RPS", "4.5"},
             {"RESEND_API_BASE", "http://127.0.0.1:9999/"},
             {"AZMAIL_PUBLIC_API_URL", "https://api.example.com///"},
             {"AZMAIL_LOCAL_DOMAINS", "Example.COM, team.example.com"},
             {"AZMAIL_SECRET", "hex:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"},
             {"AZMAIL_SIGNED_URL_TTL_SEC", "60"},
             {"AZMAIL_WS_QUEUE_CAP", "8"},
             {"AZMAIL_LOG_LEVEL", "WARN"}};
  auto r = app::config_from_env(env);
  CHECK(r.problems.empty());
  CHECK(r.warnings.empty());
  const Config& c = r.cfg;
  CHECK(c.listen_address == "0.0.0.0");
  CHECK(c.listen_port == 8443);
  CHECK(c.cors_origins == std::vector<std::string>{"https://a.example", "https://b.example"});
  CHECK(c.trusted_proxies == std::vector<std::string>{"10.0.0.0/8", "::1"});
  CHECK(c.inflight_cap == 64);
  CHECK(c.upload_body_limit == 10u << 20);
  CHECK(c.json_body_limit == 512u << 10);
  CHECK(c.draft_body_limit == 2u << 20);
  CHECK(c.inbound_raw_limit == 1u << 30);
  CHECK(c.unroutable_to_admins);
  CHECK(c.allow_insecure_http);
  CHECK(c.schedule_mode == ScheduleMode::Local);
  CHECK(c.blob_backend == BlobBackend::R2);
  CHECK(c.files_delivery == FilesDelivery::Redirect);
  CHECK(c.resend_rate_rps == 4.5);
  CHECK(c.resend_api_base == "http://127.0.0.1:9999");
  CHECK(c.public_api_base_url == "https://api.example.com");
  CHECK(c.local_domains == std::vector<std::string>{"example.com", "team.example.com"});
  CHECK(c.server_secret.size() == 32);
  CHECK(c.server_secret[1] == '\x11');
  CHECK(c.signed_url_ttl_sec == 60);
  CHECK(c.ws_queue_cap == 8);
  CHECK(c.log_level == "warn");
}

TEST_CASE("config: unparseable values are problems, the default is kept; secrets never echoed", "[config]") {
  EnvMap env{{"AZMAIL_PORT", "eighty"},
             {"AZMAIL_MAX_INFLIGHT", "-1"},
             {"AZMAIL_UPLOAD_LIMIT", "25X"},
             {"AZMAIL_UNROUTABLE_TO_ADMINS", "maybe"},
             {"AZMAIL_SCHEDULE_MODE", "later"},
             {"AZMAIL_BLOB_BACKEND", "s3"},
             {"AZMAIL_FILES_DELIVERY", "cdn"},
             {"AZMAIL_LOG_LEVEL", "loud"},
             {"RESEND_RATE_RPS", "fast"},
             {"AZMAIL_SECRET", "hex:zz-not-hex-supersecretvalue"},
             {"AZMAIL_PORT_TYPO", "1"},
             {"R2_BUCKETS", "x"},
             {"UNRELATED_VAR", "ignored silently"}};
  auto r = app::config_from_env(env);
  CHECK(r.problems.size() == 10);
  CHECK(any_contains(r.problems, "AZMAIL_PORT: not a valid integer"));
  CHECK(any_contains(r.problems, "'eighty'"));
  CHECK(any_contains(r.problems, "AZMAIL_SCHEDULE_MODE"));
  CHECK(any_contains(r.problems, "AZMAIL_SECRET"));
  CHECK_FALSE(any_contains(r.problems, "supersecretvalue"));
  CHECK(r.cfg.listen_port == Config{}.listen_port);
  CHECK(r.cfg.inflight_cap == Config{}.inflight_cap);
  REQUIRE(r.warnings.size() == 2);
  CHECK(any_contains(r.warnings, "AZMAIL_PORT_TYPO"));
  CHECK(any_contains(r.warnings, "R2_BUCKETS"));
}

TEST_CASE("config: derived defaults", "[config]") {
  auto r = app::config_from_env({{"AZMAIL_DATA_DIR", "/srv/azm"}, {"AZMAIL_PORT", "9000"}});
  CHECK(r.cfg.db_path == "/srv/azm/azmail.db");
  CHECK(r.cfg.public_api_base_url == "http://127.0.0.1:9000");
  r = app::config_from_env({{"AZMAIL_DATA_DIR", "/srv/azm"}, {"AZMAIL_DB_PATH", "/db/x.db"},
                            {"AZMAIL_LISTEN_ADDRESS", "::"}, {"AZMAIL_PUBLIC_API_URL", "https://api.x"}});
  CHECK(r.cfg.db_path == "/db/x.db");
  CHECK(r.cfg.public_api_base_url == "https://api.x");
  r = app::config_from_env({{"AZMAIL_LISTEN_ADDRESS", "::"}});
  CHECK(r.cfg.public_api_base_url == "http://[::1]:8080");
  r = app::config_from_env({});
  CHECK(r.cfg.public_api_base_url == Config{}.public_api_base_url);
  CHECK(r.cfg.db_path == Config{}.db_path);
}

TEST_CASE("config: secret decoding", "[config]") {
  CHECK(app::decode_secret("plain-secret") == std::optional<std::string>("plain-secret"));
  CHECK(app::decode_secret("hex:6869") == std::optional<std::string>("hi"));
  CHECK(app::decode_secret("HEX:6869") == std::optional<std::string>("hi"));
  CHECK(app::decode_secret("base64:aGk=") == std::optional<std::string>("hi"));
  CHECK_FALSE(app::decode_secret("hex:xyz"));
  CHECK_FALSE(app::decode_secret("base64:***"));
}

TEST_CASE("config: precedence file < process env < overrides", "[config]") {
  const EnvMap file{{"AZMAIL_PORT", "1"}, {"AZMAIL_DATA_DIR", "file"}, {"AZMAIL_LOG_LEVEL", "debug"}};
  const EnvMap process{{"AZMAIL_PORT", "2"}, {"AZMAIL_DATA_DIR", "env"}};
  const EnvMap overrides{{"AZMAIL_PORT", "3"}};
  auto merged = app::merge_env(file, process, overrides);
  CHECK(merged.at("AZMAIL_PORT") == "3");
  CHECK(merged.at("AZMAIL_DATA_DIR") == "env");
  CHECK(merged.at("AZMAIL_LOG_LEVEL") == "debug");

  // load_config (contract): real environment variables win over --env-file values.
  ScopedEnv port("AZMAIL_PORT", "4242");
  ScopedEnv other("SOMETHING_ELSE", "x");
  const auto env = app::process_environment();
  CHECK(env.at("AZMAIL_PORT") == "4242");
  CHECK(env.count("SOMETHING_ELSE") == 0);
  Config c = load_config({{"AZMAIL_PORT", "1"}, {"AZMAIL_DATA_DIR", "from-file"}});
  CHECK(c.listen_port == 4242);
  CHECK(c.data_dir == "from-file");
  {
    ScopedEnv bad("AZMAIL_MAX_CONNECTIONS", "lots");
    CHECK_THROWS_AS(load_config({}), std::invalid_argument);
  }
}

TEST_CASE("config: validation (serve)", "[config]") {
  CHECK(validate_config(valid_serve_config()).empty());

  Config c = valid_serve_config();
  c.server_secret.clear();
  c.resend_api_key.clear();
  auto p = validate_config(c);
  CHECK(any_contains(p, "AZMAIL_SECRET is required"));
  CHECK(any_contains(p, "RESEND_API_KEY"));

  c = valid_serve_config();
  c.server_secret = "short";
  CHECK(any_contains(validate_config(c), "too short"));

  c = valid_serve_config();
  c.cors_origins = {"https://ok.example", "https://bad.example/path", "*"};
  c.trusted_proxies = {"10.0.0.0/8", "10.0.0.0/33", "not-an-ip"};
  c.listen_address = "localhost";
  c.public_api_base_url = "ftp://x";
  p = validate_config(c);
  CHECK(any_contains(p, "'https://bad.example/path'"));
  CHECK(any_contains(p, "'*'"));
  CHECK(any_contains(p, "'10.0.0.0/33'"));
  CHECK(any_contains(p, "'not-an-ip'"));
  CHECK(any_contains(p, "AZMAIL_LISTEN_ADDRESS"));
  CHECK(any_contains(p, "AZMAIL_PUBLIC_API_URL"));
  CHECK_FALSE(any_contains(p, "'https://ok.example'"));

  c = valid_serve_config();
  c.resend_api_base = "http://127.0.0.1:9999";
  CHECK(any_contains(validate_config(c), "AZMAIL_ALLOW_INSECURE_HTTP"));
  c.allow_insecure_http = true;
  CHECK(validate_config(c).empty());

  c = valid_serve_config();
  c.blob_backend = BlobBackend::R2;
  p = validate_config(c);
  CHECK(any_contains(p, "R2_ACCOUNT_ID"));
  CHECK(any_contains(p, "R2_ACCESS_KEY_ID"));
  CHECK(any_contains(p, "R2_SECRET_ACCESS_KEY"));
  CHECK(any_contains(p, "R2_BUCKET"));
  c.r2_account_id = "acct";
  c.r2_access_key_id = "AK";
  c.r2_secret_access_key = "SK";
  c.r2_bucket = "azmail-blobs";
  CHECK(validate_config(c).empty());
  c.r2_bucket = "Bad_Bucket";
  CHECK(any_contains(validate_config(c), "R2_BUCKET may only contain"));
  c.r2_bucket = "azmail-blobs";
  c.r2_endpoint = "http://127.0.0.1:9000";
  CHECK(any_contains(validate_config(c), "R2_ENDPOINT uses http://"));
  c.allow_insecure_http = true;
  CHECK(validate_config(c).empty());
  c.r2_endpoint = "https://x.example/path";
  CHECK(any_contains(validate_config(c), "R2_ENDPOINT must be"));

  c = valid_serve_config();
  c.default_undo_send_seconds = 7;
  c.max_recipients_per_field = 51;
  c.schedule_max_days = 31;
  c.resend_rate_rps = 0;
  c.resend_webhook_secret = "nope";
  c.r2_presign_ttl_sec = 0;
  c.ws_queue_cap = 0;
  c.io_threads = 0;
  c.log_level = "chatty";
  p = validate_config(c);
  CHECK(any_contains(p, "AZMAIL_UNDO_SEND_SECONDS"));
  CHECK(any_contains(p, "AZMAIL_MAX_RECIPIENTS"));
  CHECK(any_contains(p, "AZMAIL_SCHEDULE_MAX_DAYS"));
  CHECK(any_contains(p, "RESEND_RATE_RPS"));
  CHECK(any_contains(p, "whsec_"));
  CHECK(any_contains(p, "R2_PRESIGN_TTL_SEC"));
  CHECK(any_contains(p, "AZMAIL_WS_QUEUE_CAP"));
  CHECK(any_contains(p, "AZMAIL_IO_THREADS"));
  CHECK(any_contains(p, "AZMAIL_LOG_LEVEL"));
  // Every problem is reported in one pass, never the secret's value.
  CHECK(p.size() >= 9);
}

TEST_CASE("config: offline validation ignores secrets and network settings", "[config]") {
  Config c;  // no secret, no Resend key
  c.cors_origins = {"garbage"};
  CHECK(app::validate_config(c, app::ConfigPurpose::Offline).empty());
  CHECK_FALSE(app::validate_config(c, app::ConfigPurpose::Serve).empty());
  c.data_dir = " ";
  c.default_undo_send_seconds = 3;
  auto p = app::validate_config(c, app::ConfigPurpose::Offline);
  CHECK(any_contains(p, "AZMAIL_DATA_DIR"));
  CHECK(any_contains(p, "AZMAIL_UNDO_SEND_SECONDS"));
}

TEST_CASE("config: warnings", "[config]") {
  Config c = valid_serve_config();
  c.db_pool_size = 4;
  c.cors_origins.clear();
  auto w = app::config_warnings(c);
  CHECK(any_contains(w, "AZMAIL_DB_POOL_SIZE"));
  CHECK(any_contains(w, "RESEND_WEBHOOK_SECRET"));
  CHECK(any_contains(w, "AZMAIL_CORS_ORIGINS"));
  c = valid_serve_config();
  c.resend_webhook_secret = "whsec_abc";
  CHECK(app::config_warnings(c).empty());
}
