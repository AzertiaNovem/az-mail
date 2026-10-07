#include "db/migrations.hpp"

#include "core/time.hpp"

#include <sqlite3.h>

#include <array>
#include <string>

namespace azm::db {
namespace {

// Migration 1: DESIGN.md §2 (including Addendum A's blobs.storage column), verbatim.
// Connection pragmas are set by db::Conn, not here.
constexpr std::string_view kMigration1 = R"SQL(
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);
CREATE TABLE kv(key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at INTEGER NOT NULL) WITHOUT ROWID; -- poll state, last_webhook_at

CREATE TABLE domains(
  id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,
  receiving_enabled INTEGER NOT NULL DEFAULT 1, created_at INTEGER NOT NULL);

CREATE TABLE users(
  id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  display_name TEXT NOT NULL DEFAULT '', password_hash TEXT NOT NULL,
  is_admin INTEGER NOT NULL DEFAULT 0, disabled INTEGER NOT NULL DEFAULT 0,
  password_changed_at INTEGER NOT NULL, last_login_at INTEGER,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);

-- single address namespace: user mailboxes + aliases
CREATE TABLE addresses(
  id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  domain_id INTEGER NOT NULL REFERENCES domains(id),
  kind TEXT NOT NULL CHECK(kind IN ('user','alias')),
  user_id INTEGER UNIQUE REFERENCES users(id) ON DELETE CASCADE,
  display_name TEXT NOT NULL DEFAULT '', share_sent INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER NOT NULL,
  CHECK((kind='user') = (user_id IS NOT NULL)));
CREATE TABLE alias_members(
  alias_id INTEGER NOT NULL REFERENCES addresses(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  can_send_as INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(alias_id,user_id)) WITHOUT ROWID;
CREATE INDEX alias_members_user ON alias_members(user_id);

CREATE TABLE sessions(
  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash BLOB NOT NULL UNIQUE, created_at INTEGER NOT NULL, last_seen_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL, user_agent TEXT NOT NULL DEFAULT '', ip TEXT NOT NULL DEFAULT '');
CREATE INDEX sessions_user ON sessions(user_id);
CREATE INDEX sessions_expires ON sessions(expires_at);

CREATE TABLE user_settings(
  user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  undo_send_seconds INTEGER NOT NULL DEFAULT 5 CHECK(undo_send_seconds IN (0,5,10,20,30)),
  signature_html TEXT NOT NULL DEFAULT '', signature_enabled INTEGER NOT NULL DEFAULT 1,
  timezone TEXT NOT NULL DEFAULT 'Asia/Shanghai',
  page_size INTEGER NOT NULL DEFAULT 50 CHECK(page_size BETWEEN 10 AND 100),
  remote_images TEXT NOT NULL DEFAULT 'ask' CHECK(remote_images IN ('ask','always')),
  updated_at INTEGER NOT NULL);
CREATE TABLE trusted_image_senders(
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  email TEXT NOT NULL COLLATE NOCASE, created_at INTEGER NOT NULL,
  PRIMARY KEY(user_id,email)) WITHOUT ROWID;

CREATE TABLE labels(
  id INTEGER PRIMARY KEY, owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name TEXT NOT NULL COLLATE NOCASE, color TEXT NOT NULL DEFAULT '#9aa0a6',
  sort_order INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL, UNIQUE(owner_id,name));

-- aggregates are ONLY written by recompute_thread()
CREATE TABLE threads(
  id INTEGER PRIMARY KEY, owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  subject TEXT NOT NULL DEFAULT '', norm_subject TEXT NOT NULL DEFAULT '',
  last_at INTEGER NOT NULL DEFAULT 0,          -- max(date) over normal msgs (incl drafts)
  spam_last_at INTEGER NOT NULL DEFAULT 0, trash_last_at INTEGER NOT NULL DEFAULT 0,
  last_message_id INTEGER, snippet TEXT NOT NULL DEFAULT '',
  participants_json TEXT NOT NULL DEFAULT '[]', -- [{name,email,unread}] ordered, max 6
  msg_count INTEGER NOT NULL DEFAULT 0, unread_count INTEGER NOT NULL DEFAULT 0,
  inbox_count INTEGER NOT NULL DEFAULT 0, inbox_unread INTEGER NOT NULL DEFAULT 0,
  starred_count INTEGER NOT NULL DEFAULT 0, sent_count INTEGER NOT NULL DEFAULT 0,
  draft_count INTEGER NOT NULL DEFAULT 0, scheduled_count INTEGER NOT NULL DEFAULT 0,
  spam_count INTEGER NOT NULL DEFAULT 0, spam_unread INTEGER NOT NULL DEFAULT 0,
  trash_count INTEGER NOT NULL DEFAULT 0, attachment_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE INDEX threads_all       ON threads(owner_id,last_at DESC,id DESC) WHERE msg_count+draft_count>0;
CREATE INDEX threads_inbox     ON threads(owner_id,last_at DESC,id DESC) WHERE inbox_count>0;
CREATE INDEX threads_starred   ON threads(owner_id,last_at DESC,id DESC) WHERE starred_count>0;
CREATE INDEX threads_sent      ON threads(owner_id,last_at DESC,id DESC) WHERE sent_count>0;
CREATE INDEX threads_drafts    ON threads(owner_id,last_at DESC,id DESC) WHERE draft_count>0;
CREATE INDEX threads_scheduled ON threads(owner_id,last_at DESC,id DESC) WHERE scheduled_count>0;
CREATE INDEX threads_spam      ON threads(owner_id,spam_last_at DESC,id DESC) WHERE spam_count>0;
CREATE INDEX threads_trash     ON threads(owner_id,trash_last_at DESC,id DESC) WHERE trash_count>0;
CREATE INDEX threads_subject   ON threads(owner_id,norm_subject,last_at);

CREATE TABLE blobs(sha256 TEXT PRIMARY KEY, size INTEGER NOT NULL,
  storage TEXT NOT NULL DEFAULT 'local' CHECK(storage IN ('local','r2')),   -- see Addendum A
  created_at INTEGER NOT NULL) WITHOUT ROWID;

CREATE TABLE inbound_emails(
  id INTEGER PRIMARY KEY, resend_id TEXT NOT NULL UNIQUE,
  state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','delivered','unroutable','failed')),
  source TEXT NOT NULL CHECK(source IN ('webhook','poll','admin')),
  message_id_header TEXT, from_email TEXT, subject TEXT, received_at INTEGER,
  raw_sha256 TEXT REFERENCES blobs(sha256), meta_json TEXT, recipients_json TEXT,
  error TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE INDEX inbound_state ON inbound_emails(state,created_at);
CREATE INDEX inbound_raw ON inbound_emails(raw_sha256);

CREATE TABLE outbound(
  id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,          -- Idempotency-Key + tag + X-AzMail-Ref
  sender_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  from_address_id INTEGER NOT NULL REFERENCES addresses(id),
  status TEXT NOT NULL CHECK(status IN ('queued','sending','accepted','scheduled','sent','delivered',
     'delivery_delayed','bounced','complained','failed','suppressed','canceled')),
  status_detail TEXT, error_name TEXT,
  send_after INTEGER NOT NULL,                  -- end of undo window
  scheduled_at INTEGER, scheduled_via TEXT CHECK(scheduled_via IN ('resend','local')),
  resend_id TEXT UNIQUE, message_id_header TEXT,
  parent_outbound_id INTEGER REFERENCES outbound(id) ON DELETE SET NULL, -- reply to own unsent-id mail
  payload_json TEXT NOT NULL,                   -- frozen request; attachments by id, no bytes
  total_bytes INTEGER NOT NULL DEFAULT 0, last_event TEXT, last_event_at INTEGER,
  job_id INTEGER, created_at INTEGER NOT NULL, accepted_at INTEGER, updated_at INTEGER NOT NULL);
CREATE INDEX outbound_status ON outbound(status,updated_at);
CREATE INDEX outbound_msgid ON outbound(message_id_header) WHERE message_id_header IS NOT NULL;

CREATE TABLE messages(
  id INTEGER PRIMARY KEY AUTOINCREMENT,         -- never reuse ids (FTS rowid, URLs, WS ids)
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
  direction TEXT NOT NULL CHECK(direction IN ('in','out')),
  is_draft INTEGER NOT NULL DEFAULT 0,
  inbound_id INTEGER REFERENCES inbound_emails(id) ON DELETE SET NULL,
  outbound_id INTEGER REFERENCES outbound(id) ON DELETE SET NULL,
  is_shared_copy INTEGER NOT NULL DEFAULT 0, sent_by_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
  from_address_id INTEGER REFERENCES addresses(id) ON DELETE SET NULL,
  from_name TEXT NOT NULL DEFAULT '', from_email TEXT NOT NULL DEFAULT '',
  to_json TEXT NOT NULL DEFAULT '[]', cc_json TEXT NOT NULL DEFAULT '[]',
  bcc_json TEXT NOT NULL DEFAULT '[]', reply_to_json TEXT NOT NULL DEFAULT '[]',
  delivered_to TEXT, subject TEXT NOT NULL DEFAULT '', snippet TEXT NOT NULL DEFAULT '',
  date INTEGER NOT NULL, message_id_header TEXT, in_reply_to TEXT,
  has_attachments INTEGER NOT NULL DEFAULT 0, size_bytes INTEGER NOT NULL DEFAULT 0,
  is_read INTEGER NOT NULL DEFAULT 0, is_starred INTEGER NOT NULL DEFAULT 0,
  in_inbox INTEGER NOT NULL DEFAULT 0, is_spam INTEGER NOT NULL DEFAULT 0, trashed_at INTEGER,
  auth_spf TEXT, auth_dkim TEXT, auth_dmarc TEXT, warnings_json TEXT NOT NULL DEFAULT '[]',
  draft_version INTEGER NOT NULL DEFAULT 0,
  draft_mode TEXT CHECK(draft_mode IN ('new','reply','reply_all','forward')),
  parent_message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  UNIQUE(owner_id,inbound_id));
CREATE INDEX messages_thread ON messages(thread_id,date);
CREATE INDEX messages_owner_date ON messages(owner_id,date DESC);
CREATE UNIQUE INDEX messages_in_msgid ON messages(owner_id,message_id_header)
  WHERE direction='in' AND message_id_header IS NOT NULL;            -- split-delivery dedupe
CREATE INDEX messages_msgid ON messages(owner_id,message_id_header) WHERE message_id_header IS NOT NULL;
CREATE INDEX messages_outbound ON messages(outbound_id) WHERE outbound_id IS NOT NULL;
CREATE INDEX messages_trashed ON messages(trashed_at) WHERE trashed_at IS NOT NULL;
CREATE INDEX messages_spam ON messages(owner_id,date) WHERE is_spam=1;
CREATE INDEX messages_from ON messages(owner_id,from_email);

CREATE TABLE message_bodies(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
  html TEXT, text TEXT, quoted_html TEXT);    -- html uses cid: for local attachments (invariant)
CREATE TABLE message_refs(                     -- In-Reply-To + References, normalized (no <>)
  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
  owner_id INTEGER NOT NULL, ref TEXT NOT NULL, PRIMARY KEY(message_id,ref)) WITHOUT ROWID;
CREATE INDEX message_refs_lookup ON message_refs(owner_id,ref);
CREATE TABLE message_labels(
  message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
  label_id INTEGER NOT NULL REFERENCES labels(id) ON DELETE CASCADE,
  PRIMARY KEY(message_id,label_id)) WITHOUT ROWID;
CREATE INDEX message_labels_label ON message_labels(label_id,message_id);

CREATE TABLE attachments(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  message_id INTEGER REFERENCES messages(id) ON DELETE CASCADE,   -- NULL = uploaded, unattached
  blob_sha256 TEXT NOT NULL REFERENCES blobs(sha256),
  filename TEXT NOT NULL, content_type TEXT NOT NULL, size INTEGER NOT NULL,
  content_id TEXT, is_inline INTEGER NOT NULL DEFAULT 0, resend_attachment_id TEXT,
  created_at INTEGER NOT NULL);
CREATE INDEX attachments_message ON attachments(message_id,id);
CREATE INDEX attachments_unattached ON attachments(created_at) WHERE message_id IS NULL;
CREATE INDEX attachments_blob ON attachments(blob_sha256);

CREATE TABLE webhook_events(
  id INTEGER PRIMARY KEY, svix_id TEXT NOT NULL UNIQUE, type TEXT NOT NULL,
  resend_email_id TEXT, payload TEXT NOT NULL, received_at INTEGER NOT NULL,
  processed_at INTEGER, result TEXT);           -- applied|enqueued|ignored_unknown|error:...
CREATE INDEX webhook_events_email ON webhook_events(resend_email_id);
CREATE INDEX webhook_events_received ON webhook_events(received_at);

CREATE TABLE delivery_events(
  id INTEGER PRIMARY KEY, outbound_id INTEGER NOT NULL REFERENCES outbound(id) ON DELETE CASCADE,
  type TEXT NOT NULL,                           -- email.delivered | local.failed | local.canceled ...
  occurred_at INTEGER NOT NULL, detail_json TEXT NOT NULL DEFAULT '{}',
  source_key TEXT NOT NULL,                     -- svix_id | 'poll:'||last_event | 'local:'||n
  UNIQUE(outbound_id,source_key));

CREATE TABLE jobs(
  id INTEGER PRIMARY KEY, kind TEXT NOT NULL, lane TEXT NOT NULL, priority INTEGER NOT NULL DEFAULT 0,
  payload TEXT NOT NULL DEFAULT '{}',
  state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','running','done','dead','canceled')),
  run_at INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 8,
  locked_until INTEGER, dedupe_key TEXT, last_error TEXT,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE INDEX jobs_ready ON jobs(lane,priority DESC,run_at,id) WHERE state='pending';
CREATE UNIQUE INDEX jobs_dedupe ON jobs(dedupe_key) WHERE dedupe_key IS NOT NULL AND state IN ('pending','running');
CREATE INDEX jobs_lease ON jobs(locked_until) WHERE state='running';
CREATE INDEX jobs_finished ON jobs(updated_at) WHERE state IN ('done','canceled');

CREATE TABLE contacts(
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  email TEXT NOT NULL COLLATE NOCASE, name TEXT NOT NULL DEFAULT '',
  score REAL NOT NULL DEFAULT 0, last_used_at INTEGER NOT NULL,   -- sent-to +1, received-from +0.2
  PRIMARY KEY(owner_id,email)) WITHOUT ROWID;

CREATE TABLE audit_log(
  id INTEGER PRIMARY KEY, actor_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
  action TEXT NOT NULL, target TEXT, detail_json TEXT, ip TEXT, at INTEGER NOT NULL);

CREATE VIRTUAL TABLE message_fts USING fts5(
  subject, from_text, to_text, body, attach_names, tokenize='trigram case_sensitive 0');
CREATE TRIGGER messages_fts_ad AFTER DELETE ON messages BEGIN
  DELETE FROM message_fts WHERE rowid = old.id; END;
)SQL";

constexpr std::array<Migration, 1> kMigrations{{
    {1, kMigration1},
}};

}  // namespace

std::span<const Migration> migrations() { return kMigrations; }

int latest_version() { return kMigrations.back().version; }

int current_version(Conn& c) {
  const auto has_table = c.scalar<int64_t>(
      "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='schema_migrations'");
  if (!has_table || *has_table == 0) return 0;
  return static_cast<int>(c.scalar<int64_t>("SELECT MAX(version) FROM schema_migrations").value_or(0));
}

int migrate(Conn& c) {
  for (const Migration& m : kMigrations) {
    c.exec("BEGIN IMMEDIATE");
    try {
      // Re-check under the write lock: another process may have migrated concurrently.
      const int cur = current_version(c);
      if (cur > latest_version())
        throw Error("database schema version " + std::to_string(cur) +
                    " is newer than this azmail binary supports (" +
                    std::to_string(latest_version()) + ")");
      if (m.version > cur) {
        c.exec(m.sql);
        c.run("INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)", m.version,
              now_ms());
      }
      c.exec("COMMIT");
    } catch (...) {
      if (c.in_transaction()) {
        try {
          c.exec("ROLLBACK");
        } catch (...) {
        }
      }
      throw;
    }
  }
  return current_version(c);
}

void check_capabilities(Conn& c) {
  const int v = sqlite3_libversion_number();
  const std::string ver = sqlite3_libversion();
  if (v < 3034000)
    throw Error("SQLite " + ver +
                " is too old: azmail needs SQLite >= 3.34 with FTS5 (trigram tokenizer). "
                "On macOS use Homebrew sqlite (SQLite3_ROOT=/opt/homebrew/opt/sqlite).");
  try {
    c.exec(
        "CREATE VIRTUAL TABLE temp.azm_fts_probe USING fts5(x, tokenize='trigram case_sensitive 0');"
        "DROP TABLE temp.azm_fts_probe;");
  } catch (const std::exception& e) {
    throw Error("SQLite " + ver +
                " lacks FTS5 with the trigram tokenizer (needed for search): " + e.what());
  }
}

}  // namespace azm::db
