#include "db/sqlite.hpp"

#include "core/log.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

namespace azm::db {
namespace {

constexpr std::size_t kMaxCachedStatements = 256;
constexpr std::size_t kSqlSnippet = 160;

std::string snippet(std::string_view sql) {
  std::string s(sql.substr(0, kSqlSnippet));
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (sql.size() > kSqlSnippet) s += "...";
  return s;
}

bool is_busy_code(int rc) {
  const int primary = rc & 0xff;
  return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

bool all_space(const char* p) {
  if (!p) return true;
  for (; *p; ++p)
    if (!(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';')) return false;
  return true;
}

// Re-entry guard: pools with an active write()/read() scope on this thread.
thread_local std::vector<const Pool*> t_active_pools;

}  // namespace

// ============================================================================================
// Stmt
// ============================================================================================

Stmt::Stmt(Stmt&& o) noexcept : owner_(o.owner_), stmt_(o.stmt_), key_(std::move(o.key_)) {
  o.owner_ = nullptr;
  o.stmt_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& o) noexcept {
  if (this != &o) {
    if (stmt_ && owner_) owner_->release(std::move(key_), stmt_);
    owner_ = o.owner_;
    stmt_ = o.stmt_;
    key_ = std::move(o.key_);
    o.owner_ = nullptr;
    o.stmt_ = nullptr;
  }
  return *this;
}

Stmt::~Stmt() {
  if (stmt_ && owner_) owner_->release(std::move(key_), stmt_);
}

Stmt& Stmt::bind(int i, Value v) {
  std::visit(
      [&](auto&& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) bind_null(i);
        else if constexpr (std::is_same_v<T, int64_t>) bind_int64(i, x);
        else if constexpr (std::is_same_v<T, double>) bind_double(i, x);
        else if constexpr (std::is_same_v<T, std::string>) bind_text(i, x);
        else bind_blob(i, std::span<const uint8_t>(x.data(), x.size()));
      },
      v);
  return *this;
}

Stmt& Stmt::bind_null(int i) {
  const int rc = sqlite3_bind_null(stmt_, i);
  if (rc != SQLITE_OK) owner_->throw_error(rc, key_);
  return *this;
}

Stmt& Stmt::bind_int64(int i, int64_t v) {
  const int rc = sqlite3_bind_int64(stmt_, i, static_cast<sqlite3_int64>(v));
  if (rc != SQLITE_OK) owner_->throw_error(rc, key_);
  return *this;
}

Stmt& Stmt::bind_double(int i, double v) {
  const int rc = sqlite3_bind_double(stmt_, i, v);
  if (rc != SQLITE_OK) owner_->throw_error(rc, key_);
  return *this;
}

Stmt& Stmt::bind_text(int i, std::string_view v) {
  // Non-null pointer even for empty strings so the value is '' rather than NULL.
  static const char kEmpty[] = "";
  const int rc = sqlite3_bind_text64(stmt_, i, v.empty() ? kEmpty : v.data(),
                                     static_cast<sqlite3_uint64>(v.size()), SQLITE_TRANSIENT,
                                     SQLITE_UTF8);
  if (rc != SQLITE_OK) owner_->throw_error(rc, key_);
  return *this;
}

Stmt& Stmt::bind_blob(int i, std::span<const uint8_t> v) {
  int rc;
  if (v.empty()) rc = sqlite3_bind_zeroblob(stmt_, i, 0);  // X'' rather than NULL
  else
    rc = sqlite3_bind_blob64(stmt_, i, v.data(), static_cast<sqlite3_uint64>(v.size()),
                             SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) owner_->throw_error(rc, key_);
  return *this;
}

int Stmt::param_index(std::string_view name) const {
  return sqlite3_bind_parameter_index(stmt_, std::string(name).c_str());
}

bool Stmt::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  owner_->throw_error(rc, key_);
}

void Stmt::run() {
  while (step()) {
  }
}

void Stmt::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

int Stmt::column_count() const { return sqlite3_column_count(stmt_); }

std::string Stmt::column_name(int c) const {
  const char* n = sqlite3_column_name(stmt_, c);
  return n ? n : "";
}

bool Stmt::is_null(int c) const { return sqlite3_column_type(stmt_, c) == SQLITE_NULL; }

int64_t Stmt::i64(int c) const { return sqlite3_column_int64(stmt_, c); }

std::optional<int64_t> Stmt::opt_i64(int c) const {
  if (is_null(c)) return std::nullopt;
  return i64(c);
}

double Stmt::dbl(int c) const { return sqlite3_column_double(stmt_, c); }

std::optional<double> Stmt::opt_dbl(int c) const {
  if (is_null(c)) return std::nullopt;
  return dbl(c);
}

std::string Stmt::text(int c) const {
  const auto* p = sqlite3_column_text(stmt_, c);
  const int n = sqlite3_column_bytes(stmt_, c);
  if (!p || n <= 0) return {};
  return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
}

std::optional<std::string> Stmt::opt_text(int c) const {
  if (is_null(c)) return std::nullopt;
  return text(c);
}

std::vector<uint8_t> Stmt::blob(int c) const {
  const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, c));
  const int n = sqlite3_column_bytes(stmt_, c);
  if (!p || n <= 0) return {};
  return std::vector<uint8_t>(p, p + n);
}

std::string Stmt::blob_str(int c) const {
  const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt_, c));
  const int n = sqlite3_column_bytes(stmt_, c);
  if (!p || n <= 0) return {};
  return std::string(p, static_cast<std::size_t>(n));
}

Value Stmt::value(int c) const {
  switch (sqlite3_column_type(stmt_, c)) {
    case SQLITE_INTEGER: return i64(c);
    case SQLITE_FLOAT: return dbl(c);
    case SQLITE_TEXT: return text(c);
    case SQLITE_BLOB: return blob(c);
    default: return nullptr;
  }
}

// ============================================================================================
// Conn
// ============================================================================================

Conn::Conn(const std::filesystem::path& path, int busy_timeout_ms) : path_(path) {
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;
  const std::string p = path.string();
  const int rc = sqlite3_open_v2(p.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    throw Error("sqlite: cannot open " + p + ": " + msg, rc & 0xff, rc);
  }
  sqlite3_extended_result_codes(db_, 1);
  try {
    set_busy_timeout(busy_timeout_ms);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA temp_store=MEMORY");
    exec("PRAGMA journal_size_limit=67108864");
  } catch (...) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
    throw;
  }
}

Conn::~Conn() {
  for (auto& [_, s] : cache_) sqlite3_finalize(s);
  cache_.clear();
  if (db_) sqlite3_close_v2(db_);
}

void Conn::throw_error(int rc, std::string_view sql) const {
  const std::string msg = std::string("sqlite: ") + (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc)) +
                          " (code " + std::to_string(rc) + ") in: " + snippet(sql);
  if (is_busy_code(rc)) throw BusyError(msg);
  throw Error(msg, rc & 0xff, rc);
}

Stmt Conn::prepare(std::string_view sql) {
  std::string key(sql);
  if (auto it = cache_.find(key); it != cache_.end()) {
    sqlite3_stmt* s = it->second;
    cache_.erase(it);
    return Stmt(this, s, std::move(key));
  }
  sqlite3_stmt* s = nullptr;
  const char* tail = nullptr;
  const int rc = sqlite3_prepare_v3(db_, key.c_str(), static_cast<int>(key.size() + 1),
                                    SQLITE_PREPARE_PERSISTENT, &s, &tail);
  if (rc != SQLITE_OK) {
    if (s) sqlite3_finalize(s);
    throw_error(rc, key);
  }
  if (!s) throw Error("sqlite: empty statement: " + snippet(key), SQLITE_MISUSE, SQLITE_MISUSE);
  if (!all_space(tail)) {
    sqlite3_finalize(s);
    throw Error("sqlite: prepare() takes a single statement (use exec): " + snippet(key),
                SQLITE_MISUSE, SQLITE_MISUSE);
  }
  return Stmt(this, s, std::move(key));
}

void Conn::release(std::string key, sqlite3_stmt* s) noexcept {
  sqlite3_reset(s);
  sqlite3_clear_bindings(s);
  if (cache_.size() >= kMaxCachedStatements) {
    sqlite3_finalize(s);
    return;
  }
  try {
    cache_.emplace(std::move(key), s);
  } catch (...) {
    sqlite3_finalize(s);
  }
}

void Conn::exec(std::string_view sql) {
  const std::string s(sql);
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err);
  if (err) sqlite3_free(err);
  if (rc != SQLITE_OK) throw_error(rc, s);
}

int64_t Conn::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }
int Conn::changes() const { return sqlite3_changes(db_); }
bool Conn::in_transaction() const { return sqlite3_get_autocommit(db_) == 0; }
void Conn::set_busy_timeout(int ms) { sqlite3_busy_timeout(db_, ms); }

// ============================================================================================
// Tx
// ============================================================================================

void Tx::emit(int64_t user_id, std::string type, boost::json::object data) {
  events_.push_back({user_id, std::move(type), std::move(data)});
}
void Tx::after_commit(std::function<void()> fn) {
  if (fn) after_.push_back(std::move(fn));
}
void Tx::wake_jobs() { wake_ = true; }

// ============================================================================================
// Pool
// ============================================================================================

struct Pool::Impl {
  std::filesystem::path path;
  std::size_t size = 1;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::unique_ptr<Conn>> idle;
  std::size_t opened = 0;  // connections created (idle + leased)
  TxHooks hooks;           // guarded by mu
};

Pool::Pool(std::filesystem::path db, std::size_t size, TxHooks hooks) : impl_(std::make_unique<Impl>()) {
  if (size == 0) throw std::invalid_argument("db::Pool: size must be >= 1");
  const std::string p = db.string();
  const bool memory = p == ":memory:" || p.empty();
  impl_->path = std::move(db);
  impl_->size = memory ? 1 : size;
  impl_->hooks = std::move(hooks);
}

Pool::~Pool() = default;

const std::filesystem::path& Pool::path() const { return impl_->path; }
std::size_t Pool::size() const { return impl_->size; }

void Pool::set_hooks(TxHooks hooks) {
  std::lock_guard lk(impl_->mu);
  impl_->hooks = std::move(hooks);
}

Pool::Lease Pool::acquire() {
  std::unique_lock lk(impl_->mu);
  for (;;) {
    if (!impl_->idle.empty()) {
      auto c = std::move(impl_->idle.back());
      impl_->idle.pop_back();
      return Lease(this, std::move(c));
    }
    if (impl_->opened < impl_->size) {
      ++impl_->opened;
      lk.unlock();
      try {
        return Lease(this, std::make_unique<Conn>(impl_->path));
      } catch (...) {
        lk.lock();
        --impl_->opened;
        impl_->cv.notify_one();
        throw;
      }
    }
    impl_->cv.wait(lk);
  }
}

void Pool::release(std::unique_ptr<Conn> c) noexcept {
  if (!c) return;
  if (c->in_transaction()) rollback(*c);
  {
    std::lock_guard lk(impl_->mu);
    try {
      impl_->idle.push_back(std::move(c));
    } catch (...) {
      --impl_->opened;  // drop the connection rather than leak the slot
    }
  }
  impl_->cv.notify_one();
}

Pool::Lease& Pool::Lease::operator=(Lease&& o) noexcept {
  if (this != &o) {
    if (pool_ && conn_) pool_->release(std::move(conn_));
    pool_ = o.pool_;
    conn_ = std::move(o.conn_);
    o.pool_ = nullptr;
  }
  return *this;
}

Pool::Lease::~Lease() {
  if (pool_ && conn_) pool_->release(std::move(conn_));
}

Pool::Scope::Scope(const Pool* p) : pool_(p) {
  if (std::find(t_active_pools.begin(), t_active_pools.end(), p) != t_active_pools.end())
    throw std::logic_error(
        "db::Pool: nested write()/read() on the same pool; use the Tx/Conn you were given");
  t_active_pools.push_back(p);
}

void Pool::Scope::release() noexcept {
  if (!active_) return;
  active_ = false;
  auto it = std::find(t_active_pools.begin(), t_active_pools.end(), pool_);
  if (it != t_active_pools.end()) t_active_pools.erase(it);
}

void Pool::begin(Conn& c, bool immediate) { c.exec(immediate ? "BEGIN IMMEDIATE" : "BEGIN DEFERRED"); }

void Pool::commit(Conn& c) { c.exec("COMMIT"); }

void Pool::rollback(Conn& c) noexcept {
  if (!c.in_transaction()) return;  // SQLite may already have rolled back (e.g. SQLITE_FULL)
  char* err = nullptr;
  sqlite3_exec(c.handle(), "ROLLBACK", nullptr, nullptr, &err);
  if (err) sqlite3_free(err);
}

void Pool::backoff(int attempt) {
  static constexpr int kDelaysMs[] = {10, 40, 160, 640};
  const int idx = std::clamp(attempt - 1, 0, 3);
  thread_local std::minstd_rand rng{std::random_device{}()};
  std::uniform_int_distribution<int> jitter(-kDelaysMs[idx] / 5, kDelaysMs[idx] / 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(kDelaysMs[idx] + jitter(rng)));
}

void Pool::flush(Tx& tx) noexcept {
  TxHooks hooks;
  {
    std::lock_guard lk(impl_->mu);
    hooks = impl_->hooks;
  }
  if (hooks.notifier) {
    for (auto& e : tx.events_) {
      try {
        hooks.notifier->publish(e.user_id, std::move(e.type), std::move(e.data));
      } catch (const std::exception& ex) {
        log::error("notifier publish failed", {{"error", ex.what()}});
      } catch (...) {
        log::error("notifier publish failed");
      }
    }
  }
  for (auto& fn : tx.after_) {
    try {
      fn();
    } catch (const std::exception& ex) {
      log::error("after_commit callback failed", {{"error", ex.what()}});
    } catch (...) {
      log::error("after_commit callback failed");
    }
  }
  if (tx.wake_ && hooks.wake_jobs) {
    try {
      hooks.wake_jobs();
    } catch (const std::exception& ex) {
      log::error("wake_jobs hook failed", {{"error", ex.what()}});
    } catch (...) {
      log::error("wake_jobs hook failed");
    }
  }
}

}  // namespace azm::db
