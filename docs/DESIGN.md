# AZ Mail — Design Specification (frozen contract)

> Source of truth for all work packages. Changes must be additive and announced. API shapes are duplicated in API.md; if they ever disagree, API.md wins for wire format.

## 0. Facts checked on this machine and in the Resend docs

These change parts of your draft:

- **Project directory**: `/Users/thiksora/Documents/az-mail` is empty. There is no git repo, so WP0 should run `git init`.
- **Boost**: 1.92 (Asio 1.38.2). In `/opt/homebrew/include/boost/beast/ssl/ssl_stream.hpp`, `beast::ssl_stream` is marked deprecated: "Use asio::ssl::stream instead". Use `asio::ssl::stream<beast::tcp_stream>`, which also works on 1.83.
- **SQLite**: Homebrew SQLite 3.53.4 is keg-only at `/opt/homebrew/opt/sqlite`. The `sqlite3` on PATH is the system 3.51. CMake must be given `SQLite3_ROOT=/opt/homebrew/opt/sqlite`, and the server checks FTS5/trigram at startup. Ubuntu 24.04 ships 3.45.1, which has trigram.
- **CMake**: 4.4 no longer has the FindBoost module, so use `find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json url program_options)`. Catch2 is 3.16 here and 3.4 on Ubuntu, both fine.
- **Resend scheduling**: the current schedule docs allow up to 30 days. The 2024 launch blog (updated 2026-07) still says "Emails with attachments cannot be scheduled". The current send-email docs don't mention that limit, so treat it as unverified. The design falls back to local scheduling.
- **Resend headers and tags**: overriding Message-ID is not documented. Tag name and value may only contain ASCII letters, digits, `_` and `-`, up to 256 characters.

---

## 1. Critique of the draft (flaw → fix)

### A. Runtime and concurrency

- **A1. "Result posted back to the session" is not specified, and it is easy to get wrong.**
  - Each connection coroutine runs on its own strand: `co_spawn(make_strand(ioc), session, detached)`, and the stream is built on that strand.
  - To hand off work, use `co_await co_spawn(pool, [&]()->awaitable<Response>{ co_return dispatch(req); }, use_awaitable)`. The caller resumes on its own strand and exceptions come back to it.
  - Do not use `co_await post(pool, use_awaitable)`. It does *not* move the coroutine onto the pool, because the handler's associated executor wins.
  - The pool never touches the stream.
  - Use two pools: `db` (8 threads) and `net` (2 threads, for the few handlers that call Resend synchronously, such as cancel and reschedule). Slow Resend calls then cannot starve DB requests.
  - Keep an atomic in-flight cap (default 256) and return 503 above it.
  - Handlers return a `Response` struct with no Beast types. The session adds CORS and security headers and writes a `string_body` or `file_body`.

- **A2. Body limits.**
  - Beast's defaults are 1 MB for the body and 8 KB for headers.
  - Read headers with `request_parser<empty_body>` (16 KB header limit), match the route, then move-construct `request_parser<string_body|file_body>` from it with that route's limit.
  - If Content-Length is over the limit, return 413 before reading the body.
  - Uploads stream to disk through `file_body`.
  - `tcp_stream::expires_after` is a deadline for the whole operation, not an idle timeout. Read large bodies in an `async_read_some` loop and re-arm 30 s on each chunk.

- **A3. A "synchronous Beast HTTPS client" has no timeouts.** Beast's sync operations ignore `expires_after`, so a stalled server hangs the worker forever.
  - Fix: build a sync facade. Each call gets a private `io_context`, runs `co_spawn(ioc, do_request(), use_future)`, then `ioc.run()`, with async operations under `expires_after`.
  - TLS: `SSL_set_tlsext_host_name` (SNI) plus `ssl::host_name_verification`.
  - Make the CA file configurable.
  - On cross-host redirects (S3 presigned URLs), drop `Authorization`.
  - Never send `Accept-Encoding`.
  - Allow `http://` only when `AZMAIL_ALLOW_INSECURE_HTTP=1`, for the mock.

- **A4. WebSocket.** Keep first-message auth, and add:
  - a 5 s auth deadline;
  - a check of `Origin` against the CORS allowlist (browsers don't apply CORS to WS);
  - `timeout::suggested(role_type::server)` for pings;
  - a 16 KiB `read_message_max`;
  - a per-session write queue on the strand, capped at 512 messages (overflow closes the socket and the client resyncs);
  - `Hub::revoke_session` / `revoke_user` on logout, password change and disable;
  - Nginx `Upgrade` headers and `proxy_read_timeout 1h`.

  Events are invalidation **hints**, not data, so dropped events are harmless. The client polls counts every 60 s while the socket is down.

- **A5. SQLite write contention.**
  - Every write transaction is `BEGIN IMMEDIATE`. A deferred read that upgrades to a write gets `SQLITE_BUSY_SNAPSHOT`, and `busy_timeout` does **not** retry that.
  - On BUSY, retry the whole closure up to 5 times with backoff.
  - Never do network I/O inside a transaction. Inbound mail downloads first, then commits in one short transaction.
  - Update a session's sliding expiry at most once per hour, otherwise every GET becomes a write.
  - Use a connection pool rather than `thread_local`, so tests can open several DBs per process.
  - Pragmas: WAL, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`, `temp_store=MEMORY`, `journal_size_limit=64MB`.
  - A daily job runs `PRAGMA optimize` and `wal_checkpoint(TRUNCATE)`.
  - The DB lives on a local disk, never NFS.

- **A6. The job queue is underspecified.** Needed:
  - **Lanes**: `outbound`, `inbound`, `sync`, `maintenance`, each with its own threads.
  - **Leases**: `locked_until`. Expired `running` jobs go back to `pending` at startup and every minute, for crash recovery.
  - **Dedupe**: `dedupe_key` with a partial UNIQUE index over pending and running jobs, so the webhook and the poller can both enqueue `inbound:<id>`.
  - **Condvar wake-up** after commit. Undo-send needs precision of about 100 ms, which polling won't give.
  - **Priority** column.
  - **429 retries don't count** toward attempts.
  - **Periodic jobs** re-enqueue themselves inside the same transaction that marks them done. Doing it earlier conflicts with the partial unique index while the job is still `running`.
  - **Cleanup**: purge done jobs after 7 days.

- **A7. Shutdown.**
  - `signal_set` on SIGINT/SIGTERM closes the acceptor, sends `going_away` to WebSockets, stops runners (up to 25 s), joins pools, then stops the ioc.
  - systemd: `TimeoutStopSec=30`.

### B. Resend integration

- **B1. Idempotency-Key = "local message uuid" breaks undo → edit → send.** The second send has the same key with a different body and gets `409 invalid_idempotent_request`.
  - Fix: every click on Send creates a new `outbound` row with a new `uuid`, and that uuid is the key.
  - Retries must stop within the key's 24 h window. The outbound backoff schedule totals about 16 h, then the send is marked `failed`.

- **B2. Message-ID capture can race with inbound replies.** Auto-responders and fast replies can arrive before `GET /emails/{id}` returns `message_id`, and for scheduled mail it stays null until the send happens.
  - Order-independent threading: a `message_refs(owner, ref)` table is checked in both directions. When a message arrives, or when a message_id is captured later, any existing messages that *reference* it get their threads merged.
  - Capture message_id from whichever source arrives first: (a) a webhook's `data.message_id`, (b) the `outbound.fetch_meta` job, (c) the loopback copy of internal mail.
  - Stamp every send with `X-AzMail-Ref: <outbound uuid>`. It survives as a header on internal loopback copies, which lets us recognise our own mail.
  - When sending a reply, resolve the parent's message_id at send time. If it's missing, fetch it with `GET /emails/{parent}` first.

- **B3. "Delivery events per recipient" is wrong.** Resend events are per email; `data.to` holds every recipient. Fix:
  - Keep a per-outbound event log.
  - Status is monotone by rank: queued < sending < accepted|scheduled < sent < delivery_delayed < delivered < complained < bounced|failed|suppressed.
  - `canceled` is local-only and terminal.
  - `opened` and `clicked` are logged but don't change status.
  - Show the bounce text as the detail.

- **B4. Webhook before commit.** `email.sent` can arrive before `resend_id` is committed (or after a crash). Send the tag `azmail_outbound=<uuid>`; webhook tags arrive as a map, so correlate by `resend_id`, falling back to the tag.

- **B5. Scheduled send through Resend.** The attachments limit is possible, message_id is unknown until send time, and cancel can race with sending. Fix: an outbound column `scheduled_via`:

  | Value | When | How it works |
  |---|---|---|
  | `resend` | default when there are no attachments | `POST /emails` with `scheduled_at`; PATCH to reschedule; `POST /emails/{id}/cancel` to cancel |
  | `local` | attachments present, `AZMAIL_SCHEDULE_MODE=local`, or Resend returns a 4xx validation error on scheduling | the outbox job's `run_at` is the scheduled time (same machinery as undo-send) |

  Further rules:
  - Always send ISO-8601 UTC (`…Z`), never natural language.
  - Allowed range is now+60 s to now+30 d.
  - A cancel that loses the race returns `409 already_sent`.

- **B6. Rate limit.** The 10 rps limit is per *team* and shared with other apps on the same key, so make the rate configurable (default 8) and recommend a dedicated key.
  - Token bucket with priorities: send > inbound fetch > meta/poll/reconcile. Low priority waits while tokens < 2.
  - A 429 `rate_limit_exceeded` pauses the whole bucket until `retry-after`.
  - `daily_quota_exceeded` / `monthly_quota_exceeded` mark the outbound `failed` ("发送配额已用完") with no auto-retry, plus an admin banner.
  - S3 downloads are not rate-limited.
  - A 403 with a non-JSON body (Cloudflare 1010) means a configuration bug (missing User-Agent), not a retryable error.

- **B7. Inbound edge cases.**
  - `GET receiving/{id}` may 404 right after the webhook: retry 5 times over about 10 min before giving up permanently.
  - `download_url` expires after 1 h, so **re-list attachments just before downloading**.
  - With 30-day retention, an outage longer than 30 days loses mail. The poller warns in admin when it finds IDs older than the high-water mark that it never saw.

- **B8. The list endpoint's sort order and cursor semantics are unverified.** The poller fetches the newest page, then walks pages until it hits an ID it already knows (max 20 pages). It never relies on cursor direction. The mock implements newest-first.

- **B9. A shared Resend account sends webhooks for other apps' mail.** Unknown `email_id`s are ignored and recorded as `ignored_unknown`; recipients outside local domains are unroutable.

- **B10. Webhooks may be unreachable** (for example an intranet-only API). The poller plus `outbound.reconcile` (`GET /emails/{id}.last_event` for recent non-terminal outbound) mean correctness doesn't depend on webhooks; they only reduce latency.

### C. Mail semantics and data model

- **C1. Routing by `(to ∪ cc ∪ bcc ∪ received_for) ∩ local` is wrong.** It lets a forged `To:` header deliver into any mailbox, and it breaks on split SMTP deliveries.
  - Fix: the envelope is the truth. Use `received_for ∩ local`, and fall back to `(to ∪ cc) ∩ local` only when `received_for` is empty.
  - Match local parts case-insensitively and strip `+tag`.
  - Split deliveries are deduplicated per owner by the inbound Message-ID unique index.

- **C2. BCC privacy.**
  - The sender's own copy keeps its BCC list.
  - Inbound copies **never** store Resend's `bcc[]`. If the owner wasn't in To/Cc, their copy stores just `bcc:[self]` so the UI can show "密送给我".
  - Alias shared-sent copies drop BCC.
  - BCC is indexed in FTS only for the sender's own copy.

- **C3. Alias fan-out vs send-as.**
  - The inbound copy records `delivered_to` (e.g. `support@`). Reply defaults From to `support@` if the user has `can_send_as`.
  - The server enforces send-as permission (403 `send_as_forbidden`).
  - When a member sends as an alias with `share_sent=1`, the other members get an outbound copy in their own thread. All copies point at one `outbound` row, so status updates touch one row and recompute N threads. Without this, other members see the customer's reply but not the team's answer.
  - Mail sent to yourself or an alias you belong to loops back with `X-AzMail-Ref`. Instead of inserting a duplicate, that owner's existing outbound copy gets `in_inbox=1`.

- **C4. Unknown local recipient = silent loss.** Resend accepts it and inbound marks it unroutable, while the sender sees "delivered". Validate local-domain recipients against `addresses` when queueing (`422 unknown_local_recipient`). Unroutable inbound mail is visible in admin. An optional policy can deliver it to admins instead.

- **C5. Status stored on each message is wrong once a send has several copies.** Status moves to `outbound`; the message rows join to it.

- **C6. "Thread list = GROUP BY over filtered messages" is O(mailbox) per page.** Denormalize the `threads` aggregates per view.
  - `recompute_thread(tx, id)` rebuilds them **from source** after any change. Threads are small, so this is cheap, and the aggregates can't drift the way incremental counters do.
  - Each folder gets a partial index, so list queries are index range scans.
  - Search still runs at message level and groups by thread (result sets are small).

- **C7. The subject + participants + 30-day fallback merges unrelated mail** (weekly "周报" from the same person). Use the fallback only when:
  - the subject has a reply prefix (Re/Fw/Fwd/回复/答复/转发/回覆/轉寄, with `:`, `：` or `[n]`);
  - and no references matched;
  - and the window is 7 days;
  - and participants overlap.

  Reference-based joins don't require a matching subject.

- **C8. Inbound fan-out idempotency.**
  - An `inbound_emails` row keyed by `resend_id`, with a state machine.
  - A `UNIQUE(owner_id, inbound_id)` index on messages.
  - A partial UNIQUE index on inbound Message-ID per owner.
  - Blobs are content-addressed and written before the transaction.
  - Delivery is a single transaction using `INSERT OR IGNORE` plus `changes()` checks.
  - Retrying the whole job at any point is safe.

- **C9. Time.**
  - DB and API carry ms-epoch UTC integers.
  - Parse Resend dates leniently: `YYYY-MM-DD[T ]HH:MM:SS[.frac][Z|±HH[:MM]]`, which covers the Postgres style (`+00`, microseconds).
  - For `before:`/`after:` search operators, the client sends `tzoff` (minutes east of UTC, i.e. `-getTimezoneOffset()`).
  - Undo countdowns use the server's *relative* `undo_ms` to avoid clock skew.
  - Users can set a timezone (default Asia/Shanghai), used for display only.
  - NTP is required for Svix's 300 s tolerance.

- **C10. Drafts.**
  - Optimistic concurrency with a `version` field; a mismatch returns 409 with the current draft.
  - `send` takes the final field values and saves and sends atomically, so a slow autosave can't drop the last edits.
  - **Stored-HTML invariant**: stored HTML always references local attachments as `cid:<content_id>` (images carry `data-att-id`). Reads rewrite to signed URLs; draft writes rewrite signed URLs back to `cid:`.
  - The quoted original is kept **outside** TipTap in `quoted_html`, because ProseMirror normalization mangles arbitrary email HTML. It is appended when the send is frozen.

- **C11. Spoofing.** Mark as spam with a warning when `dmarc=fail`, or when the From domain is local and neither DKIM nor DMARC passes and there is no matching `X-AzMail-Ref` (`spoofed_internal`). The deploy docs require DMARC `p=quarantine` or stricter on the team domain.

- **C12. Size.** Resend's 40 MB limit includes base64. Enforce 25 MiB per file and 28 MiB of raw attachments per message (×4/3 plus HTML stays under 40 MB) at queue time (`413 message_too_large`), not only at upload. Recipients are capped at 50 each for To, Cc and Bcc.

- **C13. Display names.** `"张三 (运营)" <a@b>` needs RFC 5322 quoting and escaping. Use one shared `format_address()`. RFC 2047 encoding is left to Resend.

### D. Security

- **D1. scrypt pitfall.** `EVP_PBE_scrypt` with N=2^15, r=8 needs about 32 MiB plus overhead, which **exceeds OpenSSL's default `maxmem` (32 MB), so it fails**. Pass `maxmem = 64 MiB`. Encoding: `$scrypt$ln=15,r=8,p=1$<b64salt>$<b64hash>`.
- **D2. Logins and sessions.**
  - Throttle failures: 5 per 15 min per email, 20 per 15 min per IP, then 429 with `retry_after`.
  - At most 4 concurrent scrypt operations, to prevent CPU DoS.
  - Password change revokes the other sessions; disabling a user revokes all of them and closes their WebSockets.
  - Trust `X-Forwarded-For` only from `AZMAIL_TRUSTED_PROXIES`.
- **D3. Iframe sandbox and CSP.**
  - `sandbox="allow-same-origin allow-popups allow-popups-to-escape-sandbox"`, with **no** `allow-scripts`. Same-origin is only there so the parent can measure height and intercept `mailto:`. It is safe because no script can run inside.
  - A `<meta http-equiv=CSP>` inside srcdoc: `default-src 'none'; img-src data: <api-origin> [https: http: if allowed]; style-src 'unsafe-inline'; font-src data:`.
  - srcdoc frames **inherit the parent's CSP**, so the app CSP must allow `img-src https:`. The inner meta CSP does the actual restricting.
- **D4. Serving attachments from the API origin.**
  - Always send `X-Content-Type-Options: nosniff`.
  - `inline` only for `image/{png,jpeg,gif,webp,avif,bmp}`. PDF is allowed inline without a sandbox because Chrome's viewer breaks under one.
  - Everything else is `attachment` with `Content-Security-Policy: sandbox`.
  - `image/svg+xml` and `text/html` are never inline.
  - `Content-Disposition` uses RFC 6266 `filename*=UTF-8''…` (Chinese filenames).
- **D5. Signed URLs.**
  - HMAC over `kind|id|user_id|exp|disposition`.
  - Verify that the user is active and owns the attachment.
  - TTL 12 h.
  - Never log query strings for `/api/files`.
- **D6. IDOR.** Every domain function takes `owner_id` explicitly, and every query filters on it. E2E includes cross-user access tests.
- **D7. Admins see metadata only, never mail bodies.**

### E. Frontend

- **E1. No fonts or icons from a CDN.** Google Fonts is blocked in China. Self-host Material Symbols (the `material-symbols` npm package, Outlined only; optionally subset it at build time). Use a system CJK font stack (`"PingFang SC","Microsoft YaHei","Noto Sans SC",system-ui`).
- **E2. API base at runtime.** `/config.js` sets `window.__AZMAIL_CONFIG__={apiBase}` and is served per environment by Nginx, so one build works everywhere.
- **E3. Pasted images.** Images pasted as base64 `data:` URIs are huge and many clients block them. Intercept paste and drop, upload the file as an inline attachment, and insert `<img data-att-id>`.
- **E4. Token storage.** The token lives in `localStorage` (multi-tab logout via the `storage` event). This is acceptable only with a strict app CSP (`script-src 'self'`) and the iframe isolation from D3.

### F. Build and ops

- **F1. Boost 1.83 vs 1.92 API drift.**
  - Always pass completion tokens explicitly (`use_awaitable`, `as_tuple(use_awaitable)`).
  - Define `BOOST_ASIO_NO_DEPRECATED`.
  - Use no `io_context::work`, `deadline_timer`, `beast::ssl_stream` or `rfc2818_verification`.
  - CI builds in an `ubuntu:24.04` container (WP-G).
- **F2. Compile time.** Use `BOOST_ASIO_SEPARATE_COMPILATION` and `BOOST_BEAST_SEPARATE_COMPILATION` with one `asio_impl.cpp`, plus a precompiled header for asio/beast/json.
- **F3. Backup order.** Back up the DB first (online backup API), then rsync blobs. That way the DB never references a blob missing from the backup.
- **F4. Spike with a real Resend account** (`tools/resend_probe.py`, run manually):
  1. `received_for` and split-delivery behaviour;
  2. whether the `headers` map contains in-reply-to, references and `X-*`;
  3. list order and cursor semantics;
  4. scheduled send with attachments;
  5. whether a custom `Message-ID` header is honoured;
  6. when `message_id` becomes available;
  7. whether `from`/`subject` are already decoded;
  8. which API-key permission the receiving endpoints need (assume full access);
  9. whether an empty subject is accepted.

---

## 2. Final SQLite schema (`backend/src/db/migrations.cpp`, migration 1)

Connection pragmas are set by the pool, not by the migration: WAL, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=5000`, `temp_store=MEMORY`. All `*_at` columns are ms epoch UTC.

```sql
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
```

**Keeping FTS in sync.** `rowid == messages.id`. The application calls `mail::fts_reindex(tx, message_id)` inside the same transaction whenever a message is inserted, a draft is saved, a send is frozen, an undo happens, or attachments change. The call does `DELETE` then `INSERT … SELECT`. Field contents:
- `from_text` = name + email;
- `to_text` = to + cc (+ bcc on the sender's own copy only);
- `body` = text, or `html_to_text(html)` when text is null, truncated to 256 KB;
- `attach_names` = filenames joined.

Deletes go through the trigger. `azmail reindex` rebuilds everything. FTS covers drafts too.

**Search queries.** Terms of at least 3 characters use `m.id IN (SELECT rowid FROM message_fts WHERE message_fts MATCH ?)`, with column filters and each term double-quoted. Terms of 1–2 characters (common in Chinese, e.g. 周报) use `LIKE '%t%'` on the FTS columns, which is a scan. That is acceptable up to about 100k messages per user. Negation becomes `NOT IN`.

**`recompute_thread` semantics.** "Normal" means `trashed_at IS NULL AND is_spam=0`.

| Aggregate | Counts messages that are… |
|---|---|
| `msg_count` | normal, not draft |
| `unread_count` | normal, not draft, unread |
| `inbox_count` / `inbox_unread` | normal, in inbox (/ and unread) |
| `starred_count` | normal, starred |
| `draft_count` | normal, draft |
| `sent_count` | normal, `out`, not draft, outbound status not `scheduled`/`canceled`, and not (`queued`/`sending` with `scheduled_at`) |
| `scheduled_count` | normal, `out`, outbound has `scheduled_at` and status in (`queued`, `sending`, `accepted`, `scheduled`) |
| `spam_count`, `spam_unread`, `trash_count` | the corresponding sets |
| `last_at`, `spam_last_at`, `trash_last_at` | max date of the corresponding set |

`snippet` and `last_message_id` come from the latest normal non-draft message. A thread with zero messages is deleted.

---

## 3. Backend source tree and key interfaces

`backend/` is the CMake project root. There is one static library, `azmail_lib`, built with `GLOB_RECURSE CONFIGURE_DEPENDS src/*.cpp` minus `main.cpp`; one executable, `azmail`; and `azmail_tests` from `tests/unit/*.cpp`. Because of the globs, no work package ever edits CMake. Presets: `mac-debug` (ASan/UBSan, `SQLite3_ROOT`/`OPENSSL_ROOT_DIR` from brew) and `linux-release`.

```
backend/
  CMakeLists.txt  CMakePresets.json  cmake/Warnings.cmake  cmake/pch.hpp
  src/main.cpp                      CLI dispatch: serve|migrate|create-user|reset-password|add-domain|backup|reindex|doctor
  src/config.hpp / app/config.cpp   Config struct; env (AZMAIL_*) + --env-file + flags; validation
  src/services.hpp                  Services aggregate passed to handlers/jobs
  src/notifier.hpp                  Notifier interface (implemented by ws::Hub)
  src/app/app.{hpp,cpp}             wiring: ioc, pools, db::Pool, BlobStore, Hub, Runner, signals, start/stop
  src/app/cli.cpp                   migrate/create-user (password from stdin)/backup (sqlite3_backup)/reindex/doctor
  src/core/asio_impl.cpp            Asio + Beast separate-compilation TU
  src/core/log.{hpp,cpp}            leveled logger to stderr (journald), request id
  src/core/errors.hpp               ApiError{status,code,message,details}
  src/core/time.{hpp,cpp}           now_ms, Clock, iso8601_utc, parse_iso8601_lenient
  src/core/crypto.{hpp,cpp}         random, b64/b64url, sha256 (+stream), hmac, ct_equal, uuid_v4, scrypt hash/verify
  src/core/json.{hpp,cpp}           parse with limits; req_/opt_ field getters throwing ApiError(400,'invalid_field')
  src/core/address.{hpp,cpp}        parse/format RFC5322 addresses & lists, normalize_email (+tag strip), domain_of
  src/core/strings.{hpp,cpp}        trim/lower/split, utf8_truncate, url/rfc5987 encode
  src/core/blob_store.{hpp,cpp}     BlobStore interface + LocalBlobStore (data/blobs/ab/cd/<sha>; tmp; atomic rename) — see Addendum A
  src/storage/s3_sigv4.{hpp,cpp}    pure AWS SigV4 signing (header auth + presigned query) — WP-C, Addendum A
  src/storage/r2_blob_store.{hpp,cpp} R2BlobStore : BlobStore over net::HttpClient (PUT/GET/HEAD/DELETE/presign) — WP-C
  src/storage/file_cache.{hpp,cpp}  bounded on-disk LRU cache for proxy delivery mode — WP-C
  src/core/signed_url.{hpp,cpp}     HMAC signed file/raw URLs (sign/verify)
  src/db/sqlite.{hpp,cpp}           Conn/Stmt (prepared cache)/Tx/Pool; write()/read() helpers; busy retry
  src/db/migrations.{hpp,cpp}       embedded SQL migrations; FTS5 trigram capability check
  src/http/types.hpp                Request, Response, Principal, Ctx, Route, enums
  src/http/router.{hpp,cpp}         pattern routes (/api/threads/:id), 404/405 matching
  src/http/server.{hpp,cpp}         acceptor/listener, connection cap
  src/http/session.{hpp,cpp}        per-connection coroutine: header-first parse, limits, preflight, dispatch, write, ws upgrade
  src/http/dispatch.{hpp,cpp}       on blocking pool: auth (Bearer → session), admin check, call handler, ApiError → JSON
  src/http/blocking.hpp             run_blocking(pool, f) helper
  src/http/cors.{hpp,cpp}           allowlist, preflight, Vary: Origin
  src/http/throttle.{hpp,cpp}       LoginThrottle (per email/IP sliding window) + scrypt semaphore
  src/ws/hub.{hpp,cpp}              Hub : Notifier — registry user→sessions, publish, revoke
  src/ws/ws_session.{hpp,cpp}       WS: Origin check, auth deadline, read loop, strand write queue
  src/repo/accounts.{hpp,cpp}       users, sessions, addresses/aliases/members, domains, settings, labels, audit
  src/mail/types.hpp                domain structs (below)
  src/mail/mailbox.{hpp,cpp}        list_threads, get_thread, get_message, counts, thread/message actions, contacts
  src/mail/threads.{hpp,cpp}        assign_thread, merge_threads, recompute_thread, normalize_subject
  src/mail/drafts.{hpp,cpp}         create/update/delete draft, cid<->signed rewrite on write, queue_send, undo_send
  src/mail/outbound.{hpp,cpp}       send plan, state transitions, apply_outbound_event (precedence), set_message_id, cancel/reschedule state
  src/mail/inbound.{hpp,cpp}        InboundEmail → deliver_inbound (routing, fan-out, spam, loopback merge)
  src/mail/search.{hpp,cpp}         Gmail operator parser → SqlFilter (pure, unit-testable)
  src/mail/fts.{hpp,cpp}            fts_reindex
  src/mail/html_text.{hpp,cpp}      html_to_text, snippet (quote stripping)
  src/mail/render.{hpp,cpp}         read-time cid:→signed URL rewrite, attachment URLs
  src/mail/serde.{hpp,cpp}          JSON <-> mail structs (matches docs/API.md)
  src/mail/eml.{hpp,cpp}            raw header block parse/unfold, msg-id lists, RFC2047 (iconv fallback), Date
  src/net/http_client.{hpp,cpp}     sync facade over async Beast; TLS verify; timeouts; redirects; file sink
  src/resend/types.hpp              Resend DTOs + webhook envelope
  src/resend/client.{hpp,cpp}       typed API calls, error mapping, Idempotency-Key, User-Agent
  src/resend/rate_limiter.{hpp,cpp} priority token bucket + pause_until
  src/resend/svix.{hpp,cpp}         signature verification
  src/jobs/jobs.{hpp,cpp}           enqueue/cancel/reschedule (SQL), Runner (lanes, leases, wake, backoff, periodic)
  src/jobs/kinds.hpp                kind/lane constants
  src/jobs/outbound_jobs.cpp        outbound.send / fetch_meta / reconcile
  src/jobs/inbound_jobs.cpp         inbound.fetch (GET, raw, headers, attachments → deliver_inbound), poll.receiving
  src/jobs/webhook_dispatch.{hpp,cpp} process_webhook(tx, envelope, svix_id)
  src/jobs/maintenance_jobs.cpp     purge.trash, gc.blobs, gc.housekeeping, db.optimize
  src/api/routes.{hpp,cpp}          register_routes(Router&)
  src/api/auth.cpp  mail.cpp  drafts.cpp  attachments.cpp  labels.cpp  settings.cpp  admin.cpp  webhooks.cpp  health.cpp
  src/api/dto.{hpp,cpp}             JSON for accounts/admin/settings types
  tests/unit/test_*.cpp             Catch2 (per-owner files, see §7)
  tests/fixtures/*.eml              header-parsing fixtures (GBK subject, folded References)
```

### Key interfaces (contract headers written in WP0; implementation classes use pimpl so headers stay frozen)

```cpp
// http/blocking.hpp
template <class F>
asio::awaitable<std::invoke_result_t<F>> run_blocking(asio::thread_pool& pool, F f) {
  using R = std::invoke_result_t<F>;
  co_return co_await asio::co_spawn(pool,
      [f = std::move(f)]() mutable -> asio::awaitable<R> { co_return f(); }, asio::use_awaitable);
}  // resumes on the caller's strand; exceptions rethrown in caller

// http/types.hpp
namespace azm::http {
enum class AuthReq { None, User, Admin, Signed, Webhook };
enum class BodyMode { None, Json, Raw, File };
enum class Exec { Db, Net };
struct Principal { int64_t user_id, session_id; bool is_admin; std::string email; };
struct Request {
  beast::http::verb method; std::string target, path; boost::urls::url url;
  beast::http::fields headers; std::string body; std::optional<std::filesystem::path> body_file;
  std::size_t body_size = 0; std::string remote_ip, request_id;
};
struct FileRef { std::filesystem::path path; };
struct Response {
  unsigned status = 200; std::string content_type = "application/json; charset=utf-8";
  std::vector<std::pair<std::string,std::string>> headers;
  std::variant<std::monostate, std::string, FileRef> body;
  static Response json(const boost::json::value&, unsigned status = 200);
  static Response no_content();
  static Response error(unsigned status, std::string_view code, std::string_view msg, boost::json::object details = {});
  static Response file(std::filesystem::path, std::string content_type, std::string disposition);
};
using Params = boost::container::flat_map<std::string, std::string>;
struct Ctx {
  const Request& req; Services& svc; Params params; std::optional<Principal> principal;
  const Principal& user() const;                       // throws ApiError(401)
  int64_t id(std::string_view param) const;            // throws ApiError(400)
  std::optional<std::string_view> query(std::string_view key) const;
  boost::json::object body_object() const;             // throws ApiError(400,'invalid_json')
};
using Handler = std::function<Response(Ctx&)>;
struct Route { beast::http::verb method; std::string pattern; AuthReq auth; BodyMode body;
               std::size_t body_limit; Exec exec; Handler handler; };
class Router { public:
  void add(Route);
  struct Match { const Route* route = nullptr; Params params; bool path_exists = false; };
  Match match(beast::http::verb, std::string_view path) const; };
}
// api/routes.hpp
void register_routes(azm::http::Router&);

// notifier.hpp
struct Notifier { virtual ~Notifier() = default;
  virtual void publish(int64_t user_id, std::string type, boost::json::object data) = 0;
  virtual void revoke_session(int64_t session_id) = 0;
  virtual void revoke_user(int64_t user_id) = 0; };

// db/sqlite.hpp
namespace azm::db {
struct BusyError : std::runtime_error { using runtime_error::runtime_error; };
using Value = std::variant<std::nullptr_t, int64_t, double, std::string, std::vector<uint8_t>>;
class Stmt { public:
  template <class... A> Stmt& bind_all(A&&...);        // 1-based; optional<T> → NULL
  Stmt& bind(int i, Value v);
  bool step();                                          // true = row
  int64_t i64(int c) const; std::optional<int64_t> opt_i64(int c) const;
  std::string text(int c) const; std::optional<std::string> opt_text(int c) const;
  std::vector<uint8_t> blob(int c) const; bool is_null(int c) const; };
class Conn { public:
  Stmt prepare(std::string_view sql);                   // cached per connection
  void exec(std::string_view sql);
  template <class... A> void run(std::string_view sql, A&&...);              // exec with binds
  template <class T, class... A> std::optional<T> scalar(std::string_view, A&&...);
  int64_t last_insert_id() const; int changes() const; };
struct TxHooks { Notifier* notifier = nullptr; std::function<void()> wake_jobs; };
class Tx { public:
  Conn& conn();
  void emit(int64_t user_id, std::string type, boost::json::object data);   // published after COMMIT
  void after_commit(std::function<void()>);
  void wake_jobs(); };                                                       // queued until COMMIT
class Pool { public:
  Pool(std::filesystem::path db, std::size_t size, TxHooks hooks);
  class Lease; Lease acquire();
  template <class F> auto write(F&& f) -> std::invoke_result_t<F, Tx&>;     // BEGIN IMMEDIATE; retry f on BUSY ×5; COMMIT; flush hooks
  template <class F> auto read(F&& f)  -> std::invoke_result_t<F, Conn&>; }; // BEGIN DEFERRED snapshot
}

// resend/client.hpp
namespace azm::resend {
struct Error : std::exception {
  enum class Kind { RateLimited, Quota, Validation, Auth, NotFound, IdempotencyConflict,
                    IdempotencyInFlight, Server, Network } kind;
  int http_status = 0; std::string name, message; std::optional<std::chrono::seconds> retry_after;
  bool retryable() const; };
struct OutAttachment { std::string filename, content_type, content_b64; std::optional<std::string> content_id; };
struct SendRequest { std::string from; std::vector<std::string> to, cc, bcc, reply_to;
  std::string subject, html, text; std::vector<std::pair<std::string,std::string>> headers, tags;
  std::vector<OutAttachment> attachments; std::optional<std::string> scheduled_at_iso; std::string idempotency_key; };
struct SentEmail { std::string id; std::optional<std::string> message_id, last_event;
                   std::optional<int64_t> scheduled_at_ms; int64_t created_at_ms = 0; };
struct Auth { std::optional<std::string> spf, dkim, dmarc; };
struct RecvAttachment { std::string id, filename, content_type, content_disposition;
  std::optional<std::string> content_id; int64_t size = 0; std::string download_url; };
struct ReceivedEmail { std::string id, from, subject, message_id; std::vector<std::string> to, cc, bcc, reply_to, received_for;
  std::optional<std::string> html, text; std::vector<std::pair<std::string,std::string>> headers;
  Auth auth; std::optional<std::string> raw_download_url; int64_t created_at_ms = 0;
  std::vector<RecvAttachment> attachments; };
struct ReceivedPage { std::vector<std::string> ids; bool has_more = false; };
enum class Priority { High, Normal, Low };
class Client { public:
  Client(const Config&, net::HttpClient&, RateLimiter&);
  std::string send(const SendRequest&);                               // returns id
  SentEmail get(std::string_view id, Priority = Priority::Low);
  void update_schedule(std::string_view id, std::string_view iso);
  void cancel(std::string_view id);
  ReceivedEmail get_received(std::string_view id);                    // html_format=cid
  ReceivedPage list_received(int limit, std::optional<std::string> after, std::optional<std::string> before);
  std::vector<RecvAttachment> list_received_attachments(std::string_view id);
  int64_t download(std::string_view url, const std::filesystem::path& dest, std::size_t max_bytes); // no auth header
private: struct Impl; std::unique_ptr<Impl> impl_; };
}
// resend/svix.hpp
enum class SvixResult { Ok, MissingHeaders, BadTimestamp, BadSignature };
SvixResult verify_svix(std::string_view whsec, std::string_view id, std::string_view ts,
                       std::string_view sig_header, std::string_view body, int64_t now_s, int tolerance_s = 300);

// net/http_client.hpp
struct HttpRequest { beast::http::verb method = beast::http::verb::get; std::string url;
  std::vector<std::pair<std::string,std::string>> headers; std::string body;
  std::chrono::milliseconds timeout{30000}; int max_redirects = 0;
  std::optional<std::filesystem::path> sink; std::size_t max_body = 50u << 20; };
struct HttpResponse { unsigned status; std::vector<std::pair<std::string,std::string>> headers; // lowercased names
  std::string body, final_url; std::optional<std::string> header(std::string_view) const; };
struct NetError : std::runtime_error { using runtime_error::runtime_error; };
class HttpClient { public: explicit HttpClient(ClientOptions); HttpResponse send(const HttpRequest&); /* thread-safe */ };

// jobs/jobs.hpp
namespace azm::jobs {
struct EnqueueOpts { int64_t run_at_ms = 0; std::optional<std::string> dedupe_key; int max_attempts = 8; int priority = 0; };
int64_t enqueue(db::Tx&, std::string_view kind, boost::json::object payload, EnqueueOpts = {}); // returns existing id on dedupe
bool cancel(db::Tx&, int64_t job_id);                      // only if pending
bool reschedule(db::Tx&, int64_t job_id, int64_t run_at_ms);
struct Retry : std::exception { std::chrono::milliseconds delay{0}; std::string reason; bool count_attempt = true; }; // delay 0 = backoff
struct Permanent : std::exception { std::string reason; };
struct Job { int64_t id; std::string kind; boost::json::object payload; int attempts, max_attempts; };
using JobFn = std::function<void(Services&, const Job&, std::stop_token)>;
class Runner { public:
  Runner(db::Pool&, Services&, RunnerConfig);
  void on(std::string kind, std::string lane, JobFn, std::optional<std::chrono::seconds> periodic = {});
  void start(); void stop(); void wake();
private: struct Impl; std::unique_ptr<Impl> impl_; };
}
```

**Mail domain contract** (`mail/*.hpp`, implemented by WP-B). Every function takes `owner_id`, and writes take `db::Tx&`.

```cpp
ThreadPage list_threads(db::Conn&, int64_t owner, const ThreadQuery&);  // {folder|label_id|q, cursor, limit, tzoff, now}
std::optional<ThreadDetail> get_thread(db::Conn&, const SignedUrls&, int64_t owner, int64_t thread_id);
std::optional<MessageView> get_message(db::Conn&, const SignedUrls&, int64_t owner, int64_t message_id);
Counts counts(db::Conn&, int64_t owner);
std::vector<int64_t> apply_thread_action(db::Tx&, int64_t owner, std::span<const int64_t> ids, ThreadAction, std::optional<int64_t> label_id);
void patch_message(db::Tx&, int64_t owner, int64_t message_id, const MessagePatch&);
Draft create_draft(db::Tx&, const SignedUrls&, int64_t owner, const DraftInput&);
Draft update_draft(db::Tx&, const SignedUrls&, int64_t owner, int64_t id, int64_t version, const DraftInput&, bool force); // ApiError 409 version_conflict
void delete_draft(db::Tx&, int64_t owner, int64_t id);
SendResult queue_send(db::Tx&, const Config&, int64_t owner, int64_t draft_id, const SendOptions&); // validate, freeze, outbound(+shared copies), enqueue
Draft undo_send(db::Tx&, const SignedUrls&, int64_t owner, int64_t message_id);   // ApiError 409 too_late
OutboundSendPlan load_send_plan(db::Conn&, int64_t outbound_id);
bool mark_sending(db::Tx&, int64_t outbound_id);                                  // conditional queued→sending
void mark_accepted(db::Tx&, int64_t outbound_id, std::string_view resend_id, bool scheduled);
void mark_failed(db::Tx&, int64_t outbound_id, std::string_view name, std::string_view detail_zh);
void apply_outbound_event(db::Tx&, const OutboundEvent&);   // precedence, delivery_events, message_id capture
void set_outbound_message_id(db::Tx&, int64_t outbound_id, std::string_view msgid); // propagate + rethread
std::optional<int64_t> find_outbound(db::Conn&, std::optional<std::string_view> resend_id, std::optional<std::string_view> uuid);
void begin_cancel_schedule(...); void finish_cancel_schedule(db::Tx&, int64_t outbound_id); // → draft
DeliveryResult deliver_inbound(db::Tx&, const InboundEmail&);   // idempotent fan-out, emits mail.new
void fts_reindex(db::Tx&, int64_t message_id);
SqlFilter compile_search(std::string_view q, int tzoff_min, int64_t now_ms);   // pure
std::string normalize_subject(std::string_view);
```

### Request lifecycle (WP-A)

1. Read the header (15 s timeout).
2. If it is a WS upgrade on `/api/ws`, hand over to `WsSession`.
3. `OPTIONS` returns the preflight response inline.
4. Match the route. On miss, 404 or 405.
5. Apply the body limit for that route mode and read the body.
6. `co_await run_blocking(pool[exec], dispatch)`. Dispatch resolves the Bearer token to sha256, calls `repo::find_session`, touches the session at most hourly, checks admin, runs the handler, and maps `ApiError` to JSON. Any other exception becomes a 500 and is logged with the request id.
7. Add CORS, `X-Request-Id`, `Cache-Control: no-store` and `nosniff`, then write.
8. Keep-alive loop.

### Outbound pipeline (WP-B state, WP-C network)

**`queue_send`**, inside one transaction:
1. Validate send-as permission, recipient counts, local recipients and size.
2. Freeze the payload: From formatted; html = body + signature + `quoted_html`, wrapped with inline styles; `data-att-id` stripped and `cid:` kept; `text = html_to_text`; headers `In-Reply-To`, `References` (parent's References + parent id, last 20) and `X-AzMail-Ref`; tag `azmail_outbound`; attachments listed by id.
3. Insert an `outbound` row with status `queued` and `send_after = now + undo` (or now if scheduled).
4. The message goes from draft to `out` with `outbound_id`; shared copies are created; `recompute_thread`; FTS.
5. Enqueue `outbound.send` with `run_at=send_after` (or `scheduled_at` for local scheduling) and `dedupe out:send:<id>`.

**`outbound.send` job** (outbound lane, high priority):
1. `mark_sending`; if it returns false, the send was canceled.
2. Resolve a parent message_id that is still missing.
3. Base64-encode attachments from blobs.
4. `client.send`.
5. In one transaction: `mark_accepted` and enqueue `outbound.fetch_meta` (+10 s, or `scheduled_at`+60 s).

Error handling:

| Error | Action |
|---|---|
| RateLimited | `Retry{retry_after, count_attempt=false}` |
| IdempotencyInFlight | Retry after 2 s |
| Network / Server | Retry with backoff 5 s, 15 s, 1 m, 5 m, 15 m, 1 h, 2 h, 4 h, 8 h |
| Quota / Validation / Auth / IdempotencyConflict | `mark_failed` + `Permanent` |
| `scheduled_via=resend` and a Validation error mentioning scheduling | switch to local scheduling and re-enqueue at `scheduled_at` |

**`undo_send`**: `UPDATE outbound SET status='canceled' WHERE id=? AND status='queued'`. If 0 rows changed, return `too_late`. Otherwise cancel the job, revert the message to a draft, delete shared copies and recompute.

### Inbound pipeline (WP-C)

**`inbound.fetch(resend_id)`**, all network work outside any transaction:
1. `get_received` (404 retried 5 times).
2. Download raw to tmp, hash it, put the blob.
3. Parse the header block (first 512 KB, unfold): Message-ID, In-Reply-To, References, X-AzMail-Ref, Auto-Submitted.
4. `list_received_attachments` (fresh URLs), download each into blobs (50 MB cap).
5. In one transaction: register blobs, `deliver_inbound`, and set `inbound_emails.state`.

**`deliver_inbound`**:
- Recipients = `received_for` (envelope) ∩ local; fall back to To/Cc. Resolve addresses to users and alias members, dedupe users, and record `delivered_to` per user.
- If `X-AzMail-Ref` matches an outbound and the owner already has a copy, set `in_inbox=1` and stop.
- Otherwise `INSERT OR IGNORE` the message. If `changes()==0`, skip. Then insert body, refs, attachments; spam/warnings; `assign_thread`; `recompute_thread`; FTS; contacts; `tx.emit(owner, "mail.new", …)`.
- For an internal loopback whose outbound is still missing its message_id, call `set_outbound_message_id`.

---

## 4. REST API

### Conventions
- JSON, UTF-8. IDs are numbers; times are ms epoch.
- Errors are `{"error":{"code":"snake_case","message":"中文说明","details":{}}}`.
- Auth is `Authorization: Bearer <token>`, except signed and webhook routes.
- Body limits: JSON 1 MiB (drafts 8 MiB), uploads 25 MiB, webhook 1 MiB.

| Method | Path | Auth | Request → Response |
|---|---|---|---|
| POST | /api/auth/login | – | `{email,password}` → `{token,expires_at,user:Me}`; 401 `invalid_credentials`; 429 `too_many_attempts{retry_after}` |
| POST | /api/auth/logout | U | → 204 (revokes session and its WS) |
| GET | /api/auth/me | U | → `Me` |
| POST | /api/auth/password | U | `{current_password,new_password}` → 204 (revokes other sessions) |
| GET / PUT | /api/settings | U | `Settings` ↔ `Settings` (partial PUT) |
| GET | /api/identities | U | → `Identity[]` |
| GET | /api/threads | U | `?folder=inbox\|starred\|scheduled\|sent\|drafts\|all\|spam\|trash` or `&label_id=` or `&q=`, plus `&cursor&limit&tzoff` → `ThreadListResponse` |
| GET | /api/threads/:id | U | → `ThreadDetail` (404 if not owner) |
| POST | /api/threads/actions | U | `{thread_ids:number[],action,label_id?}` → `{thread_ids}`. Actions: `archive, inbox, read, unread, star, unstar, trash, restore, spam, not_spam, delete_forever, add_label, remove_label` |
| GET | /api/messages/:id | U | → `Message` |
| PATCH | /api/messages/:id | U | `{is_read?,is_starred?,add_label_ids?,remove_label_ids?}` → `Message` |
| GET | /api/messages/:id/events | U | → `{events:[{type,occurred_at,detail}]}` |
| GET | /api/messages/:id/raw | U | → `text/plain` .eml (inbound only) |
| POST | /api/messages/:id/undo-send | U | → `{draft:Draft}`; 409 `too_late` |
| POST | /api/messages/:id/cancel-schedule | U (net pool) | → `{draft:Draft}`; 409 `already_sent`; 502 `resend_error` |
| POST | /api/messages/:id/reschedule | U (net pool) | `{scheduled_at}` → `Message` |
| POST | /api/messages/:id/retry | U | (status failed) → 202 `SendResult` (new outbound uuid) |
| POST | /api/drafts | U | `DraftInput` → 201 `Draft` |
| GET / PUT / DELETE | /api/drafts/:id | U | PUT `DraftInput & {version,force?}` → `Draft`; 409 `version_conflict{current:Draft}` |
| POST | /api/drafts/:id/send | U | `{version, draft?:DraftInput, scheduled_at?:number\|null}` → 202 `SendResult`; 403 `send_as_forbidden`; 422 `unknown_local_recipient\|too_many_recipients\|invalid_schedule`; 413 `message_too_large` |
| POST | /api/attachments | U | raw body; `?filename=<pct-enc>&inline=0\|1`; Content-Type = file type → 201 `Attachment` |
| GET | /api/files/:id | Signed | `?d=i\|a&exp&sig` → file (`nosniff`, RFC 6266, `sandbox` CSP for non-safe types) |
| GET | /api/files/raw/:messageId | Signed | → .eml download |
| GET / POST / PATCH / DELETE | /api/labels[/:id] | U | `{name,color,sort_order?}` ↔ `Label` |
| GET | /api/counts | U | → `{inbox_unread,drafts,scheduled,spam_unread,labels:{[id]:{unread,total}}}` |
| GET | /api/contacts | U | `?q&limit=8` → `{items:[{name,email,kind:'team'\|'alias'\|'contact'}]}` |
| GET | /api/ws | WS | first message auth (protocol below) |
| POST | /api/webhooks/resend | Svix | raw → 200 `{ok:true}`; 401 on bad signature or timestamp |
| GET | /api/health | – | → `{status,version,db,time}` |
| GET / POST | /api/admin/users | A | POST `{email,display_name,password,is_admin?}` → 201 `AdminUser` (domain must exist) |
| PATCH / DELETE | /api/admin/users/:id | A | `{display_name?,is_admin?,disabled?,password?}`; deleting self or the last admin → 409 |
| GET / POST | /api/admin/aliases | A | `{email,display_name,share_sent,members:[{user_id,can_send_as}]}` → `AdminAlias` |
| PATCH / DELETE | /api/admin/aliases/:id | A | members list replaced wholesale |
| GET / POST / DELETE | /api/admin/domains[/:id] | A | `{name}`; GET `/:id/status` (net pool) proxies Resend `GET /domains` |
| GET | /api/admin/events | A | `?type&cursor` → webhook events |
| GET | /api/admin/inbound | A | `?state=unroutable\|failed` |
| GET | /api/admin/outbox | A | `?status=failed\|queued\|sending` ; POST `/:id/retry` |
| GET | /api/admin/jobs | A | `?state=dead` ; POST `/:id/retry` |
| POST | /api/admin/sync | A | enqueue `poll.receiving` → 202 |
| GET | /api/admin/stats | A | `{users,messages,storage_bytes,queue:{pending,dead},sent_24h,received_24h,failed_24h,last_webhook_at,last_poll_at,quota_blocked}` |

### Main shapes (`frontend/src/api/types.ts` mirrors these exactly)

```ts
type Address = { name: string; email: string };
type OutboundStatus = 'queued'|'sending'|'accepted'|'scheduled'|'sent'|'delivered'|'delivery_delayed'|'bounced'|'complained'|'failed'|'suppressed'|'canceled';
interface Me { id:number; email:string; display_name:string; is_admin:boolean; settings:Settings; identities:Identity[];
  server:{ files_origins:string[]; blob_backend:'local'|'r2'; version:string } }   // Addendum A
interface Identity { address_id:number; email:string; display_name:string; kind:'user'|'alias'; is_default:boolean }
interface Settings { undo_send_seconds:0|5|10|20|30; signature_html:string; signature_enabled:boolean; timezone:string; page_size:number; remote_images:'ask'|'always'; trusted_image_senders:string[]; display_name:string }
interface ThreadListItem {
  id:number; subject:string; snippet:string;
  participants:{name:string; email:string; is_me:boolean; unread:boolean}[];
  message_count:number; draft_count:number; unread:boolean; starred:boolean; has_attachments:boolean;
  label_ids:number[]; last_at:number; in_inbox:boolean;
  latest_status:OutboundStatus|null; scheduled_at:number|null;
  attachments_preview:{id:number; filename:string; content_type:string}[];   // max 3
}
interface ThreadListResponse { items:ThreadListItem[]; next_cursor:string|null; total:number|null } // total null for search
interface Attachment { id:number; filename:string; content_type:string; size:number; inline:boolean; content_id:string|null; download_url:string; view_url:string|null }
interface Message {
  id:number; thread_id:number; direction:'in'|'out'; is_draft:boolean;
  from:Address; sent_by:Address|null;               // shared alias copy: actual sender
  to:Address[]; cc:Address[]; bcc:Address[]; reply_to:Address[]; delivered_to:string|null;
  subject:string; snippet:string; date:number;
  html:string|null; text:string|null;               // cid: already rewritten to signed URLs
  attachments:Attachment[];
  is_read:boolean; is_starred:boolean; in_inbox:boolean; is_spam:boolean; trashed:boolean; label_ids:number[];
  auth:{spf:string|null; dkim:string|null; dmarc:string|null}|null;
  warnings:('dmarc_fail'|'spoofed_internal')[];
  outbound:{ id:number; status:OutboundStatus; status_detail:string|null; scheduled_at:number|null;
             scheduled_via:'resend'|'local'|null; undo_until:number|null; sent_at:number|null }|null;
  message_id_header:string|null; raw_url:string|null;
}
interface ThreadDetail { id:number; subject:string; label_ids:number[]; messages:Message[] } // includes drafts & trashed; UI filters
interface DraftInput { mode?:'new'|'reply'|'reply_all'|'forward'; parent_message_id?:number|null; from_address_id?:number;
  to?:Address[]; cc?:Address[]; bcc?:Address[]; subject?:string; html?:string; quoted_html?:string|null;
  attachment_ids?:number[]; include_parent_attachments?:boolean }
interface Draft { id:number; thread_id:number; version:number; mode:DraftInput['mode']; parent_message_id:number|null;
  from_address_id:number; to:Address[]; cc:Address[]; bcc:Address[]; subject:string;
  html:string; quoted_html:string|null; attachments:Attachment[]; updated_at:number }
interface SendResult { message_id:number; thread_id:number; outbound_id:number; status:OutboundStatus; undo_ms:number; scheduled_at:number|null }
interface Label { id:number; name:string; color:string; sort_order:number }
interface AdminUser { id:number; email:string; display_name:string; is_admin:boolean; disabled:boolean; created_at:number;
  last_login_at:number|null; message_count:number; storage_bytes:number; aliases:{id:number; email:string; can_send_as:boolean}[] }
interface AdminAlias { id:number; email:string; display_name:string; share_sent:boolean; created_at:number;
  members:{user_id:number; email:string; display_name:string; can_send_as:boolean}[] }
```

### Search grammar (server is authoritative)
- `from: to: cc: bcc: subject: label: filename:`
- `in:inbox|sent|drafts|spam|trash|starred|scheduled|anywhere`
- `is:unread|read|starred`
- `has:attachment`
- `after: before:` (YYYY/MM/DD or YYYY-MM-DD in `tzoff`)
- `newer_than: older_than:` (Nd, Nm, Ny)
- `larger: smaller:` (10K, 5M)
- `"phrases"`, `-negation`, implicit AND, `OR` between two bare terms

The default scope excludes spam and trash.

### WebSocket protocol

Client → server:
- `{"type":"auth","token":"…"}`, within 5 s;
- `{"type":"ping"}`.

Server → client:

| Message | Contents |
|---|---|
| `ready` | `{user_id, server_time}` |
| `pong` | – |
| `mail.new` | `{thread_id, message_id, from, subject, snippet, in_inbox, is_spam}` |
| `threads.changed` | `{thread_ids}` |
| `outbound.status` | `{message_id, thread_id, outbound_id, status, status_detail}` |
| `labels.changed` | – |
| `settings.changed` | – |
| `session.revoked` | – |

Close code 4401 means the auth failed or timed out.

---

## 5. Frontend

### Tree (owners in brackets)

```
frontend/  package.json vite.config.ts tsconfig*.json eslint.config.js index.html public/config.js  [WP0]
src/
  main.tsx  config.ts  i18n/zh.ts (folder/status names)  styles/index.css (+mail.css [E], compose.css [F])  test/setup.ts   [WP0]
  router.tsx  [E]
  api/ types.ts client.ts endpoints.ts queryKeys.ts  [WP0, then E]   api/admin.ts [F]
  ws/ socket.ts (connect/auth/backoff 1→30s, state) invalidate.ts (event→cache, 300ms coalescing)  [E]
  stores/ auth.ts ui.ts (selection, sidebar, density) toast.ts (undo toasts, global)  [E]
          compose.ts  [WP0 contract+impl, then F]
  lib/ sanitize.ts emailFrame.ts format.ts (zh-CN dates) searchQuery.ts subject.ts keyboard.ts color.ts  [E]
       recipients.ts (reply/reply-all/forward calc, address parse) quote.ts emailHtml.ts upload.ts (XHR progress)  [F]
  components/common/ Icon Button IconButton Tooltip DropdownMenu Dialog Checkbox Spinner Avatar ToastHost  [WP0]
  components/layout/ AppShell TopBar SearchBox AdvancedSearch Sidebar LabelNavItem  [E]
  components/mail/ ThreadList ThreadRow ListToolbar Pager ThreadView ThreadToolbar MessageItem MessageHeader
                   EmailFrame PlainTextBody RemoteImagesBanner AttachmentList AttachmentPreview DeliveryStatus
                   LabelChip LabelMenu MoveToMenu EmptyState  [E]
  components/compose/ ComposeDock ComposeWindow RecipientField IdentitySelect Editor EditorToolbar
                      extensions/AttachmentImage.ts AttachmentBar QuotedToggle SendButton SchedulePicker
                      useAutosave.ts useDraftSend.ts  [F]
  components/settings/* components/admin/*  [F]
  pages/ LoginPage MailPage NotFound [E]   SettingsPage admin/{AdminLayout,Users,Aliases,Domains,Events,Outbox,Stats} [F]
```

### Routes (react-router v7, data mode, `createBrowserRouter`)

- `/` → `/mail/inbox`
- `/login`
- `/mail/:folder` and `/mail/:folder/:threadId`
- `/mail/label/:labelId[/:threadId]`
- `/mail/search[/:threadId]?q=`
- `/settings/:tab(general|labels|account)`
- `/admin/:tab(users|aliases|domains|events|outbox|stats)` (requires `is_admin`)
- `*` → NotFound

`RequireAuth` loads `['me']`. Opening a thread that contains only drafts opens the compose window instead. Nginx uses `try_files … /index.html`.

### Query keys and invalidation

| Key | staleTime | Updated or invalidated by |
|---|---|---|
| `['me']` | 5 min | settings/password mutations, `settings.changed` |
| `['labels']` | 5 min | label mutations, `labels.changed` |
| `['counts']` | 15 s; `refetchInterval` 60 s while WS is not open | every mail mutation and every mail WS event (coalesced) |
| `['threads',{folder,labelId,q},cursor]` | 30 s | `mail.new`, `threads.changed`, `outbound.status` (sent/scheduled), mutations |
| `['thread',id]` | 60 s | `mail.new`/`threads.changed` with that id; `outbound.status` patches in place via `setQueryData` |
| `['draft',id]` | Infinity | owned by its compose window; never invalidated by WS |
| `['contacts',q]` | 5 min | – |
| `['admin',…]` | 0 | admin mutations |

- On WS reconnect `ready`, invalidate `threads`, `thread` and `counts`.
- `session.revoked` logs out.
- The list uses a Gmail-style pager with a cursor stack ("1–50 / 共 N"), not infinite scroll.
- Thread actions are optimistic:
  - `onMutate`: `cancelQueries(['threads'])`, snapshot, then transform every `['threads']` cache (remove the thread from the folder on archive/trash/spam; flip unread/star) and `['thread',id]`.
  - On error: roll back and show a toast.
  - Archive and trash show a "撤销" toast that runs the inverse action.
  - Opening a thread marks it read immediately.

### Compose store (zustand, contract)

```ts
type ComposeInit = {kind:'new'; to?:Address[]; subject?:string}
  | {kind:'reply'|'reply_all'|'forward'; parentMessageId:number; threadId:number}
  | {kind:'draft'; draftId:number};
interface ComposeWin { key:string; draftId:number|null; init:ComposeInit; minimized:boolean; maximized:boolean;
  saveState:'idle'|'dirty'|'saving'|'saved'|'error'|'conflict'; title:string }
interface ComposeStore { windows:ComposeWin[]; open(i:ComposeInit):string; close(k:string):void;
  toggleMinimize(k:string):void; toggleMaximize(k:string):void; focus(k:string):void; patch(k:string,p:Partial<ComposeWin>):void }
```

Behaviour:
- `open` with a draft that's already open focuses it.
- At most 3 expanded windows, docked right-to-left; the rest become minimized chips.
- Open window keys and `draftId`s persist in sessionStorage.
- Editor content lives in TipTap. `useAutosave` debounces 1.5 s and also saves on blur and close. The first save is a POST with a guard against a duplicate in-flight POST; after that, PUT with `version`. A 409 shows "已在其他窗口修改" with Reload / Overwrite (`force`).
- Reply and forward build recipients with `recipients.ts` (reply = `reply_to[0]` or `from`; reply-all = from+to+cc minus my identities). `quote.ts` builds `quoted_html` from the cached parent, sanitized, with the header "在 2026年10月7日 周三 20:03，张三 <a@b> 写道：" (forward has its own block). The draft is created lazily on the first edit.
- Send: flush autosave, POST send with the final fields, close the window, then push a global toast "邮件已发送 · 撤销 · 查看邮件" for `undo_ms`. Undo reopens the returned draft.
- Send is disabled while uploads are pending. An empty subject asks for confirmation.
- Uploads go through XHR for progress. Drop and paste of images insert `<img data-att-id src=view_url>`.
- TipTap v3: StarterKit (includes Link and Underline), Image extended with a `data-att-id` attribute, TextStyle/Color, Placeholder.
- The signature is inserted into new bodies above the quote.

### Email HTML sanitization (`lib/sanitize.ts` + `EmailFrame.tsx`)

1. Parse with DOMParser and move `<head><style>` into the body.
2. Run DOMPurify with `FORCE_BODY`.
   - `FORBID_TAGS`: script, iframe, frame, object, embed, applet, form, input, button, textarea, select, base, meta, link.
   - URI allowlist: https, http, mailto, tel, data:image/*, the API files origin.
3. In an `afterSanitizeAttributes` hook:
   - `a`: set `target=_blank rel="noopener noreferrer nofollow"`.
   - `img[src|srcset]`, `background`, `style url()`: if the source is remote and not allowed, move it to `data-azm-src`, set a 1×1 placeholder and count it.
   - In `<style>` text, strip `@import` and remote `url()` when blocked.
4. Build srcdoc: `<meta charset>` + meta CSP (img-src depends on whether remote images are allowed) + `<base target=_blank>` + base styles (`img{max-width:100%}`, `word-break`).
5. Render `<iframe sandbox="allow-same-origin allow-popups allow-popups-to-escape-sandbox" referrerpolicy="no-referrer">`. Height comes from a ResizeObserver on `contentDocument`. Clicks on `mailto:` links are intercepted to open compose.
6. If any images were blocked, show the banner "已隐藏外部图片 · 显示图片 · 始终显示来自 x 的图片" (adds to `trusted_image_senders`). Setting `remote_images=always` skips the blocking.
7. Plain-text messages are escaped and linkified in a `pre-wrap` div, not an iframe.
8. Quoted blocks (`.gmail_quote`, `blockquote[type=cite]`) collapse behind "…".
9. Emails always render on a light background.

Status labels in Chinese: 待发送, 发送中, 已发送, 已定时 {time}, 已送达, 投递延迟, 退信, 被标记为垃圾邮件, 发送失败, 已被抑制, 已取消.

Vitest (jsdom) covers sanitize (XSS vectors, remote blocking, cid URLs kept), recipients, searchQuery, compose store and quote.

---

## 6. Mock Resend server and E2E plan

### Mock: `tools/mock_resend/` (Python stdlib, `ThreadingHTTPServer`)

Files: `server.py`, `svix.py`, `eml.py` (builds MIME with the `email` package, including RFC 2047 subjects), `store.py`, `README.md`.

Flags: `--port --api-key --webhook-url --webhook-secret --local-domains --time-scale`.

API emulation:
- **Auth and edge behaviour**: Bearer key check (401 `invalid_api_key`). A missing `User-Agent` returns 403 with the non-JSON body `error code: 1010`. A 10 rps fixed window returns 429 `rate_limit_exceeded` with `retry-after`.
- **POST /emails**: validates from/to (≤50)/subject/html|text, base64 attachments, `content_id` < 128, and `scheduled_at` (ISO, within 30 days; optional `--reject-scheduled-attachments`). Idempotency: same key and body replays the same response; a different body returns 409 `invalid_idempotent_request`; an in-flight key returns 409 `concurrent_idempotent_requests`.
- **GET /emails/{id}** returns a Postgres-style `created_at` and `message_id` (nullable for `--meta-delay` seconds).
- **PATCH** and **POST /cancel** work only while scheduled; otherwise they return a 422 error.
- **Delivery simulation per recipient**:
  - Local domain: loopback. Build a raw .eml with Message-ID = the outbound's message_id, In-Reply-To/References/X-AzMail-Ref from the request's `headers`, and attachments. Store a received email with `received_for` = local envelope recipients (including bcc), headers lowercased, and `authentication` all pass. Send a signed `email.received` webhook.
  - External, by address prefix: `bounce@` → bounced, `fail@` → failed, `delay@` → delayed then delivered, `complain@` → delivered then complained, `suppress@` → suppressed, otherwise delivered.
  - Events: `email.sent`, `email.scheduled`, `email.delivered`, with `tags` as a map.
- **Receiving**: list (limit 1–100 required, newest-first, `after`/`before`, `has_more`), `GET /emails/receiving/{id}?html_format=cid`, `GET …/attachments`. Download URLs are `/_dl/<token>?exp=`, which 302-redirects to `/_blob/…`, enforces expiry and rejects an `Authorization` header (catches credential leaks).
- **Webhook sender**: Svix signing with retries (0, 1 s, 3 s).
- **Control endpoints**:
  - `POST /_mock/inbound`: inject external mail `{from,to,cc,received_for,subject,html,text,attachments,headers,dmarc}`;
  - `POST /_mock/faults`: `[{match:"POST /emails",status|timeout,name,count}]`;
  - `POST /_mock/config`: `webhooks_enabled`, `shuffle_events`, `duplicate_webhooks`, `split_delivery`, `strip_custom_headers`, `meta_delay`, `raw_missing`;
  - `POST /_mock/advance?seconds=`: fire scheduled sends;
  - `GET /_mock/sent`: requests including UA and idempotency keys;
  - `POST /_mock/reset`.
- Self-test: the Svix test vector from your brief must reproduce `v1,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0=`.

### E2E: `tests/e2e/` (stdlib only)

Files: `run.py` (random ports, temp data dir, starts mock and backend, `-k` filter, `--keep`), `lib/api.py`, `lib/ws.py` (minimal RFC 6455 client), `lib/proc.py`, `lib/wait.py` (`eventually`), `scenarios/sNN_*.py`.

| # | Scenario | Assertions |
|---|---|---|
| 01 | bootstrap: migrate, create-user admin (stdin), login, me, health | token works; doctor OK |
| 02 | admin creates domain, alice, bob, carol, dave, alias support@ (alice send-as, bob member) | 409 on duplicate address |
| 03 | alice→bob send with 5 s undo | mock has nothing before 5 s; then 1 POST with UA + Idempotency-Key; bob gets `mail.new` over WS; status → delivered |
| 04 | undo inside the window / undo after it | draft restored, mock got 0 POSTs / 409 `too_late` |
| 05 | undo → edit → resend | new idempotency key, no 409 |
| 06 | bob replies | alice's thread has 2 messages via In-Reply-To |
| 07 | race: `meta_delay=10` and reply injected before capture | threads merged after capture |
| 08 | `strip_custom_headers` + send to self | still merged by message_id; no duplicate |
| 09 | to bob, cc carol, bcc dave | dave's copy `bcc=[dave]`; bob/carol `bcc=[]`; alice's sent copy shows dave |
| 10 | external → support@ | alice and bob each get a copy with independent read state; `delivered_to=support@` |
| 11 | alice replies as support@ | bob gets a shared sent copy in the same thread; status updates reach both |
| 12 | bob tries to send as support@ | 403 `send_as_forbidden` |
| 13 | send to unknown local address | 422 `unknown_local_recipient` |
| 14 | attachments: Chinese filename + inline image | mock gets `content_id`; html has `cid:`; recipient downloads via redirect; signed URL OK; tampered/expired 403; `filename*` correct; SVG served as attachment |
| 15 | upload > 25 MiB / message > 28 MiB | 413 / 413 `message_too_large` |
| 16 | scheduled (no attachments) | `scheduled_via=resend`; reschedule PATCH; cancel → draft; advance → sent |
| 17 | scheduled with attachments + `reject-scheduled-attachments` | local path, sent at advance time |
| 18 | events: bounce/fail/delay/complain/suppress; shuffled; duplicate svix-id | final statuses correct; duplicates ignored |
| 19 | webhook security: bad signature, timestamp ±6 min, oversized | 401, 401, 413 |
| 20 | faults: 429 ×3, 500 ×2, one timeout | exactly 1 email at the mock; attempts not consumed by 429 |
| 21 | `daily_quota_exceeded` | failed, not retried; retry endpoint works after the fault clears |
| 22 | webhooks off + inject + `/admin/sync` | mail appears; poll + webhook for the same id → 1 copy |
| 23 | `split_delivery` | one copy per owner |
| 24 | dmarc fail; spoofed internal From | spam + warnings; `not_spam` moves it to inbox |
| 25 | search: 3+ Chinese chars, 2-char term, from:, has:attachment, label:, in:anywhere, before/after with tzoff, negation | expected thread ids |
| 26 | labels CRUD + thread actions (archive/star/read/trash/restore/spam/delete_forever) | counts and folders correct |
| 27 | draft version conflict | 409 with current draft; force works |
| 28 | auth: throttling 429; logout invalidates; password change revokes others and closes their WS; disabled user | – |
| 29 | IDOR: bob GETs alice's thread/message/attachment/draft | 404 everywhere |
| 30 | CORS: allowed origin preflight; disallowed origin; WS bad Origin | headers present/absent; WS rejected |
| 31 | unroutable inbound | visible in admin |
| 32 | crash: SIGKILL during undo window, restart | sent exactly once |
| 33 | purge (retention override) and blob GC | files removed |
| 34 | raw .eml (show original) | available and matches |

The browser check runs `scripts/dev.sh` (mock + backend + seed + vite) and walks through login, inbox, opening a thread, compose with an inline image, undo, scheduling, a label, search and admin user creation.

---

## 7. Work packages

**Rule:** WP0 lands first. After that, every file has exactly one owner. Contract headers and types are frozen: only additive changes, made by the owner and announced. Contract classes use pimpl. WP0 ships stubs that compile and link (throwing `not_implemented`), so every package builds from day one. CMake uses globs, so nobody edits it.

| WP | Scope (owns) | Contracts it provides | Consumes | Done when |
|---|---|---|---|---|
| **WP0 Foundation** (orchestrator, first) | git init, `.gitignore`, `docs/{ARCHITECTURE,API}.md` (§4 verbatim), `backend/CMakeLists.txt` + presets + pch; `src/core/*` fully implemented with tests (crypto incl. scrypt maxmem, svix-independent HMAC, time, json, address, strings, blob_store, signed_url, log); `src/db/sqlite.*` + `migrations.*` (schema §2) with `test_db.cpp`; all contract headers from §3 + stub `.cpp`s; `config.hpp`, `services.hpp`, `notifier.hpp`; frontend scaffold (all deps in package.json, Vite/Tailwind v4/tsconfig/eslint/vitest), `api/{types,client,endpoints,queryKeys}.ts`, `components/common/*`, `stores/compose.ts`, `main.tsx`, `config.ts`, `i18n/zh.ts`, `styles/index.css` | everything below | – | `cmake --build` and `ctest` green on macOS; `pnpm build && pnpm test` green |
| **WP-A Runtime** | `src/http/*`, `src/ws/*`, `src/app/*` (config loader, wiring, CLI), `main.cpp`; tests `test_router`, `test_cors`, `test_config`, `test_session_limits` (in-process server on an ephemeral port), `test_ws_auth` | Router impl, Hub : Notifier, `run_blocking`, LoginThrottle, CLI | repo::find_session (WP-D), `register_routes` (WP-D), Runner (WP-C) | health/login over real HTTP; 413/405/preflight; WS auth, revoke and backpressure tests |
| **WP-B Mail domain** | `src/mail/{types,mailbox,threads,drafts,outbound,inbound,search,fts,html_text,render,serde}.*`; tests `test_threading` (bidirectional refs, merge, subject fallback), `test_search`, `test_status_precedence`, `test_delivery` (envelope routing, BCC rules, alias fan-out, loopback merge, idempotency), `test_drafts` (cid invariant, version, freeze, undo race), `test_thread_list` (aggregates, cursors) | `mail/*.hpp` functions + serde | db, core, jobs::enqueue (WP0 header) | all domain tests green on an in-memory DB |
| **WP-C Resend & jobs** | `src/net/*`, `src/resend/*`, `src/jobs/*`, `src/mail/eml.*`; tests `test_svix` (test vector), `test_eml` (fixtures), `test_rate_limiter`, `test_jobs` (lease recovery, dedupe, periodic, 429 not counted), `test_resend_client` (CTest fixture starts the Python mock) | Client, HttpClient, Runner, `process_webhook`, job handlers | mail/outbound, inbound, deliver (WP-B) | jobs pass against the mock; timeouts proven; no Authorization on redirect |
| **WP-D API & accounts** | `src/repo/accounts.*`, `src/api/*`; tests `test_accounts`, `test_api_handlers` (Ctx-level calls with a fake Services) | `register_routes`, repo::* (sessions, users, aliases, …) | Router/types (WP-A contract), mail/* (WP-B), resend client + svix (WP-C) | every route in §4 implemented, error codes as listed |
| **WP-E Frontend mail** | the files marked [E] in §5 | router, ws, invalidation, sanitize | api contract, compose store | vitest green; list, thread, search, labels, WS working against the backend |
| **WP-F Frontend compose / settings / admin** | the files marked [F] in §5 | ComposeDock/Window, admin & settings pages | api contract, common components, toast store (E's interface: `toast.push`) | full compose flow incl. undo, schedule, inline images, autosave conflict; admin CRUD |
| **WP-G Mock, E2E, deploy** | `tools/mock_resend/*`, `tools/resend_probe.py` (spike script, §1 F4), `tests/e2e/*`, `scripts/{dev.sh,seed_dev.py,build_backend.sh}`, `deploy/{azmail.service,azmail.env.example,nginx-frontend.conf,nginx-api.conf,Dockerfile.ubuntu2404}`, `docs/DEPLOY.md` | mock behaviour per §6; CI container verifying Boost 1.83 | API.md, Resend facts | mock self-tests pass; E2E runnable; Ubuntu 24.04 container builds and runs unit tests |

### Dependency order

1. WP0.
2. A, B, C, D, E, F and G in parallel. Everything compiles against stubs.
3. Integration, in this order:
   - **M1**: A+D. Login, health, admin user creation.
   - **M2**: +B. Drafts and mailbox reads/actions (E2E 01, 02, 26, 27, 29).
   - **M3**: +C+G. Send, receive and status round trip (E2E 03–24).
   - **M4**: E+F against the M3 backend, then the browser check.
   - **M5**: full E2E, the Ubuntu container, and the spike run against real Resend.

### Integration contracts to hold everyone to
- the JSON shapes in `docs/API.md` and `api/types.ts`;
- the WS event names and payloads;
- job kinds, lanes and payloads (`jobs/kinds.hpp`):

  | Kind | Lane | Payload |
  |---|---|---|
  | `outbound.send` | outbound | `{outbound_id}` |
  | `outbound.fetch_meta` | sync | `{outbound_id}` |
  | `outbound.reconcile` | maintenance | periodic, 10 min |
  | `inbound.fetch` | inbound | `{resend_id, source}`, dedupe `in:<id>` |
  | `poll.receiving` | sync | periodic, `AZMAIL_POLL_INTERVAL_SEC` (default 120) |
  | `purge.trash`, `gc.blobs`, `gc.housekeeping`, `db.optimize` | maintenance | periodic |

- the stored-HTML `cid:` invariant;
- `tx.emit` only, never publishing from inside a transaction;
- `owner_id` on every domain call.

### Deploy notes for WP-G

**systemd unit**:
- `User=azmail`
- `EnvironmentFile=/etc/azmail/azmail.env`
- `ExecStart=/usr/local/bin/azmail serve`
- `Restart=on-failure`
- `NoNewPrivileges`, `ProtectSystem=strict`, `ReadWritePaths=/var/lib/azmail`, `PrivateTmp`
- `LimitNOFILE=65536`, `TimeoutStopSec=30`

**API Nginx**:
- TLS, `proxy_pass 127.0.0.1:8080`
- for `/api/ws`: an `Upgrade`/`Connection` map and `proxy_read_timeout 1h`
- `client_max_body_size 30m`
- `X-Forwarded-For`
- `gzip_proxied any`, `gzip_types application/json`

**Frontend Nginx**:
- SPA fallback
- `Cache-Control: no-store` for `/config.js` and `index.html`; immutable caching for `/assets`
- CSP: `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob: https:; connect-src 'self' <api> <wss-api>; font-src 'self'; frame-src 'self'; object-src 'none'; base-uri 'none'`

**DNS and Resend checklist**:
- MX → `inbound-smtp.us-east-1.amazonaws.com` (priority 10)
- SPF/DKIM from Resend; DMARC `p=quarantine` or stricter
- a webhook endpoint subscribed to all `email.*` events
- a full-access API key; NTP running

---

The 2024 blog statement about scheduled attachments came from [Resend: Introducing the Schedule Email API](https://resend.com/blog/introducing-the-schedule-email-api). The 30-day window and the current silence on attachments came from [Resend docs: Schedule email](https://resend.com/docs/dashboard/emails/schedule-email). Tag rules came from [Resend docs: Send email](https://resend.com/docs/api-reference/emails/send-email). Header rules came from [Resend docs: Custom headers](https://resend.com/docs/dashboard/emails/custom-headers). Message-ID availability came from the [Resend changelog: Message ID for sent emails](https://resend.com/changelog/message-id-for-sent-emails).


---

## Addendum A — Cloudflare R2 blob storage (user requirement, added before WP0 freeze)

All blobs (inbound raw `.eml`, inbound attachments, user uploads) are content-addressed by sha256 and stored through a `BlobStore` interface with two implementations:

| Backend | Use | Selected by |
|---|---|---|
| `LocalBlobStore` | dev, tests, small installs | `AZMAIL_BLOB_BACKEND=local` (default) |
| `R2BlobStore` | production (recommended) | `AZMAIL_BLOB_BACKEND=r2` |

### Config (env)
`AZMAIL_BLOB_BACKEND=local|r2`, `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `R2_BUCKET`, `R2_ENDPOINT` (optional; default `https://<account>.r2.cloudflarestorage.com`; the mock uses `http://127.0.0.1:<port>` with `AZMAIL_ALLOW_INSECURE_HTTP=1`), `R2_PREFIX` (default `azmail/`), `AZMAIL_FILES_DELIVERY=proxy|redirect` (default **`proxy`** — see A.2), `R2_PRESIGN_TTL_SEC` (default 300), `AZMAIL_FILE_CACHE_MB` (proxy mode cache, default 1024). Startup validates the bucket with a HEAD on a probe key (`doctor` does too).

### Object layout
Key = `<R2_PREFIX>blobs/<aa>/<bb>/<sha256>` (path-style URL `/<bucket>/<key>`, region `auto`, service `s3`). Objects are immutable; metadata (filename, content type) lives only in SQLite, never in R2. `blobs.storage` records where each blob lives, so a mixed store is possible and `azmail blobs-migrate --to r2|local` (CLI, WP-A) can move blobs (copy → verify sha → flip column → delete source).

### Interface (`src/core/blob_store.hpp`, WP0; frozen)
```cpp
namespace azm {
struct BlobRef { std::string sha256; int64_t size = 0; std::string storage; };   // storage = "local"|"r2"
struct LocalFile { std::filesystem::path path; };
struct RedirectUrl { std::string url; };                                         // presigned GET
using ServePlan = std::variant<LocalFile, RedirectUrl>;
struct ServeOptions { std::string content_type; std::string disposition;        // full Content-Disposition value
                      std::chrono::seconds ttl{300}; };
class BlobStore { public:
  virtual ~BlobStore() = default;
  virtual std::string_view kind() const = 0;                                     // "local" | "r2"
  virtual std::filesystem::path tmp_dir() const = 0;                             // local scratch for staging
  // Upload/move a staged temp file (caller already streamed it). Computes sha if absent. Idempotent:
  // an existing blob is not re-uploaded (HEAD first for r2). Deletes the temp file on success.
  virtual BlobRef put_file(const std::filesystem::path& staged, std::optional<std::string> sha256_hex = {}) = 0;
  virtual BlobRef put_bytes(std::string_view bytes) = 0;
  virtual void get_to_file(std::string_view sha256, const std::filesystem::path& dest) = 0;   // throws BlobNotFound
  virtual std::string get_bytes(std::string_view sha256, std::size_t max_bytes) = 0;
  virtual bool exists(std::string_view sha256) = 0;
  virtual void remove(std::string_view sha256) = 0;                              // missing = no-op
  // How to deliver a blob to a browser. Local → LocalFile. R2+redirect → presigned GET with
  // response-content-type / response-content-disposition overrides. R2+proxy → LocalFile from file_cache.
  virtual ServePlan serve(std::string_view sha256, const ServeOptions&) = 0;
  // Origins that serve file bytes to browsers (for iframe/img CSP), e.g. "https://<acct>.r2.cloudflarestorage.com".
  virtual std::vector<std::string> public_origins() const = 0;
};
struct BlobNotFound : std::runtime_error { using runtime_error::runtime_error; };
struct BlobError : std::runtime_error { using runtime_error::runtime_error; bool retryable = true; };
std::unique_ptr<BlobStore> make_local_blob_store(std::filesystem::path data_dir);
}
// src/storage/r2_blob_store.hpp (WP-C)
std::unique_ptr<azm::BlobStore> make_r2_blob_store(const azm::Config&, azm::net::HttpClient&);
```
All `BlobStore` calls are blocking network/disk I/O → never inside a transaction (register `blobs` rows *after* `put_file` succeeds, in the delivery/upload transaction). `blobs` rows are inserted with `INSERT OR IGNORE`.

### Data flows
- **Upload** (`POST /api/attachments`): session streams body to `tmp_dir()` via `file_body` (sha256 computed while streaming, size limit enforced) → handler on the **net** pool calls `put_file` → short tx inserts `blobs` + `attachments`.
- **Inbound**: Resend `download_url` → tmp file (hash while downloading) → `put_file` → delivery tx.
- **Outbound send**: `get_bytes` (≤ 25 MiB each) → base64 → Resend `attachments[].content`. (Resend `path` with a presigned URL is a possible later optimization; not used because fetch timing for scheduled mail is undocumented.)
- **Download / inline images** (`GET /api/files/:id?d=&exp=&sig=`): verify our HMAC signed URL + ownership, then `serve()`:
  - `LocalFile` → stream with `file_body` (+ nosniff, RFC 6266, sandbox CSP for unsafe types).
  - `RedirectUrl` → `302 Location: <presigned R2 URL>` + `Cache-Control: private, max-age=60`. Unsafe types are forced to `response-content-type=application/octet-stream` and `attachment` disposition. R2 is a different origin, so it is isolated from the app origin.
- **Proxy mode** (`AZMAIL_FILES_DELIVERY=proxy`, for networks where `*.r2.cloudflarestorage.com` is slow/blocked, e.g. some mainland-China networks): backend fetches into `file_cache` (LRU, size-capped) and streams like local.
- **GC** (`gc.blobs` job): blobs with zero references (attachments + inbound raw) older than 1 day → `remove()` → delete row.
- **Backup**: DB online backup only; R2 holds blobs (enable R2 bucket versioning / lifecycle as desired — documented in DEPLOY.md).

### API / frontend impact
- `Me` gains `server: { files_origins: string[] }` (API origin + `public_origins()`); the email iframe CSP `img-src` includes these origins (CSP checks redirect targets).
- Frontend app CSP (`deploy/nginx-frontend.conf`) `img-src` already allows `https:`.
- `GET /api/admin/stats` gains `storage: { backend, delivery, blob_count, blob_bytes }`.

### Ownership
- WP0: `core/blob_store.hpp` interface + `LocalBlobStore` + tests, config fields, `blobs.storage` column.
- WP-C: `storage/s3_sigv4.*` (unit-tested against the AWS SigV4 published S3 examples), `storage/r2_blob_store.*`, `storage/file_cache.*`, `net::HttpRequest` gains `std::optional<std::filesystem::path> body_file` (streamed request body) and response-to-file sink with sha256.
- WP-A: wiring (`make_*_blob_store` by config), `blobs-migrate` CLI, startup probe.
- WP-G: mock gains a minimal S3 endpoint (`PUT/GET/HEAD/DELETE /<bucket>/<key>`, SigV4 header + presigned verification, `response-*` overrides); E2E runs the whole suite with `AZMAIL_BLOB_BACKEND=r2` against the mock (plus a short smoke with `local`); scenario 35: presigned redirect works, tampered/expired presign → 403, proxy mode streams.

### A.2 R2 wire facts (researched 2026-10; drive WP-C and the mock)
- Endpoint `https://<ACCOUNT_ID>.r2.cloudflarestorage.com` (jurisdiction: `https://<ACCOUNT_ID>.eu.r2.cloudflarestorage.com`, set via `R2_ENDPOINT`). Use **path-style** `/<bucket>/<key>`. SigV4 scope `YYYYMMDD/auto/s3/aws4_request` (region `auto`, service `s3`).
- Always send `x-amz-content-sha256`: for PUT use the real hex sha256 of the body (we always know it — blobs are content-addressed); for GET/HEAD/DELETE the empty-body hash `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. **No** streaming/chunked SigV4 (unsupported) → single PUT with `Content-Length` (required; 411 otherwise). Single PUT limit ~5 GiB (we never exceed 50 MB). Objects are PUT with `Content-Type: application/octet-stream` (metadata lives in SQLite; identical bytes may have different filenames).
- Errors are S3 XML `<Error><Code>…</Code><Message>…</Message></Error>`: 403 `SignatureDoesNotMatch`/`AccessDenied`/`ExpiredRequest`, 404 `NoSuchKey`/`NoSuchBucket`, 429 `TooManyRequests` (limit **1 write/s per key** — concurrent uploads of the same content-addressed blob: treat as retryable, re-HEAD first; if it now exists, success), 503 `ServiceUnavailable` (retry with exponential backoff). Map 5xx/429/network → `BlobError{retryable=true}`, 403/400 → non-retryable (config error, log clearly without secrets).
- Presigned URLs: GET/HEAD/PUT/DELETE, expiry 1 s–7 d, **only on the S3 API host (not custom domains)**. `response-content-type` / `response-content-disposition` overrides are **undocumented on R2** (third-party reports say they work) → `redirect` mode must be validated by `azmail doctor --r2` (presign a probe object with overrides, GET it, check headers) and falls back to proxy if the check fails.
- R2 sends no security headers (no `nosniff`); plain `<img>`/`<a>` navigation to a presigned URL needs no CORS.
- Mainland China: R2 buckets can't be created in mainland China and `*.r2.cloudflarestorage.com` is reported as often slow/timing out across the border (not officially blocked). Users here are Chinese → **default delivery = `proxy`** (backend streams via `file_cache`; browser only ever talks to the API origin). `redirect` is an opt-in bandwidth optimization for users with good connectivity. Backend↔R2 traffic still crosses the border if the server is in China — DEPLOY.md recommends hosting the backend in HK/SG/JP or overseas.
- SigV4 unit-test vectors (AWS S3 docs; key `AKIAIOSFODNN7EXAMPLE` / `wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY`, 20130524T000000Z, us-east-1, s3, host `examplebucket.s3.amazonaws.com`; signing key hex `dbb893acc010964918f1fd433add87c70e8b0db6be30c1fbeafefa5ec6ba8378`):
  - GET `/test.txt` with `range:bytes=0-9`, empty-payload hash, SignedHeaders `host;range;x-amz-content-sha256;x-amz-date` → canonical-request sha `7344ae5b7ee6c3e7e6b0fe0640412a37625d1fbfff95c48bbb2dc43964946972`, signature `f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41`.
  - PUT `/test%24file.text` (key `test$file.text`), body `Welcome to Amazon S3.` (sha `44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072`), headers `date:Fri, 24 May 2013 00:00:00 GMT`, host, x-amz-content-sha256, x-amz-date, `x-amz-storage-class:REDUCED_REDUNDANCY` → canonical sha `9e0e90d9c76de8fa5b200d8c849cd5b8dc7a3be3951ddb7f6a76b4158342019d`, signature `98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd`.
  - GET `/?lifecycle` (query line `lifecycle=`), SignedHeaders `host;x-amz-content-sha256;x-amz-date` → signature `fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543`.
  - GET `/?max-keys=2&prefix=J` → signature `34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7`.
  - Presigned GET `/test.txt`, `X-Amz-Expires=86400`, SignedHeaders `host`, payload `UNSIGNED-PAYLOAD` → canonical sha `3bfa292879f6447bbcda7001decf97f4a54dc650c8942174ae0a9121cf58ad04`, signature `aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404`.
  - Encoding: URI-encode everything except `A-Za-z0-9-._~` with uppercase hex; space → `%20`; keep `/` in the path, encode it as `%2F` in query values; sort query params after encoding; do not normalize the path. Canonical request lines joined with `\n`, blank line after the headers block, no trailing newline.
