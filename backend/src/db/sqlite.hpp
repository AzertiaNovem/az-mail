// SQLite access layer: Conn / Stmt (prepared-statement cache) / Tx / Pool (DESIGN §3, A5).
//
// Rules:
//  * Writes go through Pool::write: BEGIN IMMEDIATE, the closure, COMMIT. On SQLITE_BUSY /
//    BUSY_SNAPSHOT / LOCKED the whole closure is retried (5 attempts, backoff 10/40/160/640 ms),
//    then BusyError. The closure must therefore be safe to re-run (no network I/O, no external
//    side effects; use tx.after_commit for those).
//  * Any exception rolls back and discards queued emits / after_commit callbacks.
//  * After a successful COMMIT: tx.emit events → hooks.notifier, then after_commit callbacks,
//    then hooks.wake_jobs (once, if requested). Exceptions there are logged, not rethrown.
//  * Pool::read runs the closure in BEGIN DEFERRED ... COMMIT (consistent snapshot).
//  * Do not call write()/read() on the same Pool from inside a write()/read() closure (it would
//    see a different snapshot or deadlock a small pool): std::logic_error is thrown. Use the
//    Conn / Tx you were given.
//  * A Conn (and every Stmt prepared from it) is used by one thread at a time; Stmt objects must
//    not outlive their Conn / Lease.
#pragma once

#include "notifier.hpp"

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace azm::db {

// SQLite error (message includes the sqlite message and an SQL snippet).
struct Error : std::runtime_error {
  Error(const std::string& msg, int code_ = 0, int extended_code_ = 0)
      : std::runtime_error(msg), code(code_), extended_code(extended_code_) {}
  int code = 0;           // primary result code, e.g. SQLITE_CONSTRAINT (19)
  int extended_code = 0;  // e.g. SQLITE_CONSTRAINT_UNIQUE (2067)
  bool is_constraint() const { return code == 19; }
  // UNIQUE or PRIMARY KEY violation (e.g. duplicate address → 409).
  bool is_unique_violation() const { return extended_code == 2067 || extended_code == 1555; }
  bool is_foreign_key_violation() const { return extended_code == 787; }
};

struct BusyError : std::runtime_error {
  using runtime_error::runtime_error;
};

using Value = std::variant<std::nullptr_t, int64_t, double, std::string, std::vector<uint8_t>>;

class Conn;

namespace detail {
template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};
template <class>
inline constexpr bool always_false = false;
}  // namespace detail

class Stmt {
 public:
  Stmt(Stmt&& o) noexcept;
  Stmt& operator=(Stmt&& o) noexcept;
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  ~Stmt();  // resets, clears bindings and returns the statement to its Conn's cache

  // Binds parameters 1..N. Supported: integral types (bool → 0/1, char → 1-char text),
  // float/double, std::string, std::string_view, const char* (nullptr → NULL), std::nullptr_t,
  // std::nullopt_t, std::optional<T> (empty → NULL), std::vector<uint8_t> / std::span (BLOB),
  // db::Value.
  template <class... A>
  Stmt& bind_all(A&&... a) {
    int i = 1;
    (bind_one(i++, std::forward<A>(a)), ...);
    return *this;
  }
  Stmt& bind(int i, Value v);
  Stmt& bind_null(int i);
  Stmt& bind_int64(int i, int64_t v);
  Stmt& bind_double(int i, double v);
  Stmt& bind_text(int i, std::string_view v);
  Stmt& bind_blob(int i, std::span<const uint8_t> v);
  int param_index(std::string_view name) const;  // ":name" → index (0 if unknown)

  bool step();  // true = row available; throws BusyError / Error
  void run();   // step to completion, ignoring rows
  void reset();  // reset + clear bindings (done automatically on destruction)

  int column_count() const;
  std::string column_name(int c) const;
  bool is_null(int c) const;
  int64_t i64(int c) const;  // NULL → 0
  std::optional<int64_t> opt_i64(int c) const;
  double dbl(int c) const;  // NULL → 0.0
  std::optional<double> opt_dbl(int c) const;
  bool boolean(int c) const { return i64(c) != 0; }
  std::string text(int c) const;  // NULL → ""
  std::optional<std::string> opt_text(int c) const;
  std::vector<uint8_t> blob(int c) const;  // NULL → {}
  std::string blob_str(int c) const;       // BLOB bytes as std::string
  Value value(int c) const;

  template <class T>
  T get(int c) const {
    if constexpr (std::is_same_v<T, bool>) return boolean(c);
    else if constexpr (std::is_integral_v<T>) return static_cast<T>(i64(c));
    else if constexpr (std::is_floating_point_v<T>) return static_cast<T>(dbl(c));
    else if constexpr (std::is_same_v<T, std::string>) return text(c);
    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) return blob(c);
    else if constexpr (std::is_same_v<T, Value>) return value(c);
    else static_assert(detail::always_false<T>, "unsupported column type");
  }

  std::string_view sql() const { return key_; }
  sqlite3_stmt* handle() const { return stmt_; }

 private:
  friend class Conn;
  Stmt(Conn* owner, sqlite3_stmt* s, std::string key) : owner_(owner), stmt_(s), key_(std::move(key)) {}

  template <class T>
  void bind_one(int i, T&& v) {
    using D = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<D, std::nullptr_t> || std::is_same_v<D, std::nullopt_t>) {
      bind_null(i);
    } else if constexpr (detail::is_optional<D>::value) {
      if (v) bind_one(i, *std::forward<T>(v));
      else bind_null(i);
    } else if constexpr (std::is_same_v<D, Value>) {
      bind(i, std::forward<T>(v));
    } else if constexpr (std::is_same_v<D, bool>) {
      bind_int64(i, v ? 1 : 0);
    } else if constexpr (std::is_same_v<D, char>) {
      bind_text(i, std::string_view(&v, 1));
    } else if constexpr (std::is_integral_v<D>) {
      if constexpr (std::is_unsigned_v<D> && sizeof(D) >= sizeof(int64_t)) {
        if (v > static_cast<D>(std::numeric_limits<int64_t>::max()))
          throw std::out_of_range("db bind: unsigned value exceeds int64");
      }
      bind_int64(i, static_cast<int64_t>(v));
    } else if constexpr (std::is_floating_point_v<D>) {
      bind_double(i, static_cast<double>(v));
    } else if constexpr (std::is_same_v<D, std::vector<uint8_t>>) {
      bind_blob(i, std::span<const uint8_t>(v.data(), v.size()));
    } else if constexpr (std::is_convertible_v<const D&, std::span<const uint8_t>>) {
      bind_blob(i, std::span<const uint8_t>(v));
    } else if constexpr (std::is_pointer_v<std::decay_t<D>> &&
                         std::is_convertible_v<std::decay_t<D>, const char*>) {
      const char* p = v;
      if (p) bind_text(i, std::string_view(p));
      else bind_null(i);
    } else if constexpr (std::is_convertible_v<const D&, std::string_view>) {
      bind_text(i, std::string_view(v));
    } else {
      static_assert(detail::always_false<D>, "unsupported db bind type");
    }
  }

  Conn* owner_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
  std::string key_;  // SQL text (cache key)
};

class Conn {
 public:
  // Opens (creating if needed) with SQLITE_OPEN_URI and sets the connection pragmas:
  // journal_mode=WAL, synchronous=NORMAL, foreign_keys=ON, busy_timeout, temp_store=MEMORY,
  // journal_size_limit=64 MiB.
  explicit Conn(const std::filesystem::path& path, int busy_timeout_ms = 5000);
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  Stmt prepare(std::string_view sql);  // exactly one statement; cached per connection
  void exec(std::string_view sql);     // one or more statements, no binds
  template <class... A>
  void run(std::string_view sql, A&&... a) {
    Stmt s = prepare(sql);
    s.bind_all(std::forward<A>(a)...);
    s.run();
  }
  // First column of the first row; nullopt when there is no row or the value is NULL.
  template <class T, class... A>
  std::optional<T> scalar(std::string_view sql, A&&... a) {
    Stmt s = prepare(sql);
    s.bind_all(std::forward<A>(a)...);
    if (!s.step() || s.is_null(0)) return std::nullopt;
    return s.template get<T>(0);
  }
  int64_t last_insert_id() const;
  int changes() const;

  sqlite3* handle() const { return db_; }  // raw handle (online backup, custom functions)
  const std::filesystem::path& path() const { return path_; }
  bool in_transaction() const;
  void set_busy_timeout(int ms);
  std::size_t cached_statements() const { return cache_.size(); }

  // Throws BusyError / Error for a failed result code (message from sqlite3_errmsg).
  [[noreturn]] void throw_error(int rc, std::string_view sql) const;

 private:
  friend class Stmt;
  void release(std::string key, sqlite3_stmt* s) noexcept;

  sqlite3* db_ = nullptr;
  std::filesystem::path path_;
  std::unordered_multimap<std::string, sqlite3_stmt*> cache_;
};

struct TxHooks {
  Notifier* notifier = nullptr;
  std::function<void()> wake_jobs;
};

class Tx {
 public:
  Conn& conn() { return *conn_; }
  void emit(int64_t user_id, std::string type, boost::json::object data);  // published after COMMIT
  void after_commit(std::function<void()> fn);
  void wake_jobs();  // queued until COMMIT

  // Convenience forwards to conn().
  Stmt prepare(std::string_view sql) { return conn_->prepare(sql); }
  void exec(std::string_view sql) { conn_->exec(sql); }
  template <class... A>
  void run(std::string_view sql, A&&... a) {
    conn_->run(sql, std::forward<A>(a)...);
  }
  template <class T, class... A>
  std::optional<T> scalar(std::string_view sql, A&&... a) {
    return conn_->template scalar<T>(sql, std::forward<A>(a)...);
  }
  int64_t last_insert_id() const { return conn_->last_insert_id(); }
  int changes() const { return conn_->changes(); }

  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;

 private:
  friend class Pool;
  struct Event {
    int64_t user_id;
    std::string type;
    boost::json::object data;
  };
  explicit Tx(Conn* c) : conn_(c) {}

  Conn* conn_;
  std::vector<Event> events_;
  std::vector<std::function<void()>> after_;
  bool wake_ = false;
};

class Pool {
 public:
  static constexpr int kMaxAttempts = 5;

  // Connections are opened lazily up to `size`; acquire() blocks when all are leased.
  // ":memory:" databases are per-connection, so the size is clamped to 1 for them.
  Pool(std::filesystem::path db, std::size_t size, TxHooks hooks = {});
  ~Pool();  // all leases must have been returned
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  class Lease {
   public:
    Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) { o.pool_ = nullptr; }
    Lease& operator=(Lease&& o) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease();  // rolls back an open transaction, returns the connection
    Conn& operator*() const { return *conn_; }
    Conn* operator->() const { return conn_.get(); }
    Conn& conn() const { return *conn_; }

   private:
    friend class Pool;
    Lease(Pool* p, std::unique_ptr<Conn> c) : pool_(p), conn_(std::move(c)) {}
    Pool* pool_ = nullptr;
    std::unique_ptr<Conn> conn_;
  };

  Lease acquire();

  template <class F>
  auto write(F&& f) -> std::invoke_result_t<F, Tx&>;  // BEGIN IMMEDIATE; retry f on BUSY ×5; COMMIT; flush hooks
  template <class F>
  auto read(F&& f) -> std::invoke_result_t<F, Conn&>;  // BEGIN DEFERRED snapshot

  const std::filesystem::path& path() const;
  std::size_t size() const;
  // Replace hooks (e.g. wake_jobs once the job Runner exists). Thread-safe.
  void set_hooks(TxHooks hooks);

 private:
  struct Impl;

  // Guards against write()/read() re-entry on the same Pool from one thread.
  class Scope {
   public:
    explicit Scope(const Pool* p);
    ~Scope() { release(); }
    void release() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    const Pool* pool_;
    bool active_ = true;
  };

  void release(std::unique_ptr<Conn> c) noexcept;
  static void begin(Conn& c, bool immediate);
  static void commit(Conn& c);
  static void rollback(Conn& c) noexcept;
  static void backoff(int attempt);
  void flush(Tx& tx) noexcept;

  std::unique_ptr<Impl> impl_;
};

template <class F>
auto Pool::write(F&& f) -> std::invoke_result_t<F, Tx&> {
  using R = std::invoke_result_t<F, Tx&>;
  static_assert(!std::is_reference_v<R>, "Pool::write closures must return by value");
  Scope scope(this);
  for (int attempt = 1;; ++attempt) {
    Tx tx(nullptr);
    [[maybe_unused]] std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> result{};
    try {
      Lease lease = acquire();
      tx.conn_ = &lease.conn();
      begin(lease.conn(), true);
      try {
        if constexpr (std::is_void_v<R>) {
          f(tx);
        } else {
          result.emplace(f(tx));
        }
        commit(lease.conn());
      } catch (...) {
        rollback(lease.conn());
        throw;
      }
    } catch (const BusyError&) {
      if (attempt >= kMaxAttempts) throw;
      backoff(attempt);
      continue;
    }
    scope.release();  // hooks may start new transactions on this pool
    flush(tx);
    if constexpr (!std::is_void_v<R>) return std::move(*result);
    else return;
  }
}

template <class F>
auto Pool::read(F&& f) -> std::invoke_result_t<F, Conn&> {
  using R = std::invoke_result_t<F, Conn&>;
  static_assert(!std::is_reference_v<R>, "Pool::read closures must return by value");
  Scope scope(this);
  for (int attempt = 1;; ++attempt) {
    try {
      Lease lease = acquire();
      begin(lease.conn(), false);
      try {
        if constexpr (std::is_void_v<R>) {
          f(lease.conn());
          commit(lease.conn());
          return;
        } else {
          R r = f(lease.conn());
          commit(lease.conn());
          return r;
        }
      } catch (...) {
        rollback(lease.conn());
        throw;
      }
    } catch (const BusyError&) {
      if (attempt >= kMaxAttempts) throw;
      backoff(attempt);
    }
  }
}

}  // namespace azm::db
