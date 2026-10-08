# AZ Mail — Backend contracts (WP0 output)

> Companion to `docs/DESIGN.md` (frozen spec) and `docs/API.md` (wire format). This file maps the
> spec onto the contract headers in `backend/src/**.hpp` written in WP0. Headers are frozen:
> only additive changes by the owning work package, announced to the others. Every header
> starts with `// Owner: WP-X`. Every non-trivial function is a WP0 stub that throws
> `azm::NotImplemented("<function>")`, so all packages build from day one. Exceptions, already
> implemented in WP0: inline enum/constant helpers in the headers, `http/types.cpp`,
> `api::route_table()`, `jobs::enqueue/cancel/reschedule/extend_lease`, `repo::session_token_hash`,
> `azm::blob_writer_guard/blob_gc_guard`, `SignedUrls::expiry`, `resend::ScopedStopToken`, the
> `tests/unit/test_support.hpp` fixtures, and no-op `ws::Hub` publish/revoke (§H).

Work packages: **WP0** foundation · **WP-A** runtime (http/ws/app) · **WP-B** mail domain ·
**WP-C** Resend, R2 storage, jobs, eml · **WP-D** API & accounts · **WP-G** mock/E2E/deploy.

---

## A. File → owner

### Build, shared, core, db (WP0 — implemented and tested)

| File | Owner | Notes |
|---|---|---|
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/Warnings.cmake`, `cmake/pch.hpp`, `cmake/sanitizer_defaults.cpp` | WP0 | globs; nobody else edits. WP0 added `find_package(Iconv)` (see §H) |
| `src/config.hpp` | WP0 | `Config` struct; loader `load_config` / `validate_config` implemented by WP-A in `src/app/config.cpp` |
| `src/services.hpp` | WP0 | `Services` aggregate; WP-A builds it |
| `src/notifier.hpp` | WP0 | `Notifier`, `NullNotifier`, `RecordingNotifier` |
| `src/core/*` (asio_impl, log, errors, time, crypto, json, address, strings, blob_store, signed_url) | WP0 | fully implemented |
| `src/db/sqlite.{hpp,cpp}`, `src/db/migrations.{hpp,cpp}` | WP0 | fully implemented |
| `src/db/kv.hpp` | WP0 | inline `kv` helpers + key constants (keys additive by anyone) |
| `tests/unit/test_{address,asio_impl,blob_store,crypto,db,json,log,migrations,signed_url,strings,time}.cpp`, `tests/unit/test_support.hpp` | WP0 | `test_support.hpp` = `TempDir`, `migrate`, `seed_domain/seed_user/seed_alias/seed_session`, `address_id`, `TestServices` (§H 33); usable by every package |
| `tests/unit/test_contracts_compile.cpp` | WP0 | includes every header; asserts the inline helpers |
| `docs/CONTRACTS.md` | WP0 | this file; owners append decisions (additive) |

### WP-A runtime

| File | Contents |
|---|---|
| `src/main.cpp` | CLI dispatch → `app::run_cli` (currently a WP0 placeholder) |
| `src/app/app.{hpp,cpp}` | `app::App` wiring, start/stop/run |
| `src/app/cli.{hpp,cpp}` | `app::run_cli` (serve, migrate, create-user, reset-password, add-domain, backup, reindex, doctor, blobs-migrate, version) |
| `src/app/config.cpp` | `azm::load_config`, `azm::validate_config` |
| `src/http/types.{hpp,cpp}` | Request/Response/Principal/Ctx/Route; **`types.cpp` already implemented in WP0** (helpers every test needs) |
| `src/http/router.{hpp,cpp}` | `http::Router` |
| `src/http/blocking.hpp` | `http::run_blocking` (implemented inline in WP0, frozen) |
| `src/http/server.{hpp,cpp}` | `http::Server`, `ServerDeps` |
| `src/http/session.{hpp,cpp}` | `http::run_session`, `SessionShared`, pure helpers |
| `src/http/dispatch.{hpp,cpp}` | `http::dispatch`, `bearer_token`, `authenticate` |
| `src/http/cors.{hpp,cpp}` | `CorsPolicy`, `origin_allowed`, `apply_cors`, `preflight` |
| `src/http/throttle.{hpp,cpp}` | `http::LoginThrottle` |
| `src/ws/events.hpp` | WS event names + payload builders (frozen by WP0, implemented inline) |
| `src/ws/hub.{hpp,cpp}` | `ws::Hub : Notifier`, `ws::WsSink` |
| `src/ws/ws_session.{hpp,cpp}` | `ws::run_ws_session`, `parse_client_message` |
| tests | `test_router`, `test_cors`, `test_config`, `test_session_limits`, `test_ws_auth` (+ `test_hub`, `test_throttle`, `test_cli` as needed) |

### WP-B mail domain

| File | Contents |
|---|---|
| `src/mail/types.hpp` | all domain structs/enums (+ inline enum helpers, implemented in WP0) |
| `src/mail/mailbox.{hpp,cpp}` | list/get/counts/actions/events/contacts/purge |
| `src/mail/threads.{hpp,cpp}` | assign/adopt/merge/recompute, normalize_subject |
| `src/mail/drafts.{hpp,cpp}` | drafts, queue_send, undo_send, resolve_sender |
| `src/mail/attachments.{hpp,cpp}` | **new in WP0**: blobs/attachments rows, ownership lookups, GC queries |
| `src/mail/outbound.{hpp,cpp}` | outbound state machine, cancel/reschedule/retry |
| `src/mail/inbound.{hpp,cpp}` | routing, deliver_inbound, inbound_emails state |
| `src/mail/search.{hpp,cpp}` | parse_search, compile_search |
| `src/mail/fts.{hpp,cpp}` | fts_reindex, fts_rebuild_all |
| `src/mail/html_text.{hpp,cpp}` | html_to_text, make_snippet |
| `src/mail/render.{hpp,cpp}` | cid↔signed rewrite, file serve policy, attachment views |
| `src/mail/serde.{hpp,cpp}` | JSON for mail views and request bodies |
| tests | `test_threading`, `test_search`, `test_status_precedence`, `test_delivery`, `test_drafts`, `test_thread_list` (+ `test_render`, `test_html_text`, `test_serde`) |

### WP-C Resend, storage, jobs, eml

| File | Contents |
|---|---|
| `src/net/http_client.{hpp,cpp}` | `net::HttpClient` (+ Addendum A body_file / sink sha256), `ClientOptions` |
| `src/resend/types.{hpp,cpp}` | DTOs, `Error`, `classify_error`, `WebhookEnvelope`, `parse_webhook` |
| `src/resend/client.{hpp,cpp}` | `resend::Client` (§3 + download_to, list_domains, get_domain) |
| `src/resend/rate_limiter.{hpp,cpp}` | `resend::RateLimiter` |
| `src/resend/svix.{hpp,cpp}` | `verify_svix`, `sign_svix` |
| `src/storage/s3_sigv4.{hpp,cpp}` | pure SigV4 |
| `src/storage/r2_blob_store.{hpp,cpp}` | `make_r2_blob_store`, `R2Options`, `probe_r2` |
| `src/storage/file_cache.{hpp,cpp}` | `storage::FileCache` |
| `src/jobs/jobs.{hpp,cpp}` | enqueue/cancel/reschedule (**implemented in WP0**, WP-B depends on them), `Runner` |
| `src/jobs/kinds.hpp` | kinds, lanes, priorities, dedupe keys (frozen by WP0, inline) |
| `src/jobs/webhook_dispatch.{hpp,cpp}` | `process_webhook` |
| `src/jobs/handlers.hpp` | **new in WP0**: register_* and run_* declarations |
| `src/jobs/outbound_jobs.cpp`, `src/jobs/inbound_jobs.cpp`, `src/jobs/maintenance_jobs.cpp` | handlers |
| `src/mail/eml.{hpp,cpp}` | raw header parsing, RFC 2047 via iconv |
| `tests/fixtures/*.eml` | header fixtures |
| tests | `test_svix`, `test_eml`, `test_rate_limiter`, `test_jobs`, `test_resend_client` (+ `test_s3_sigv4`, `test_r2_blob_store`, `test_file_cache`, `test_http_client`, `test_webhook_dispatch`) |

### WP-D API & accounts

| File | Contents |
|---|---|
| `src/repo/accounts.{hpp,cpp}` | users, sessions, addresses/aliases, identities, domains, settings, labels, audit, admin read models |
| `src/api/routes.{hpp,cpp}` | `route_table()` (**written in WP0** — the route contract), `register_routes` |
| `src/api/handlers.hpp` | **new in WP0**: one function per endpoint |
| `src/api/dto.{hpp,cpp}` | JSON for accounts/settings/admin + request parsers |
| `src/api/{auth,settings,mail,drafts,attachments,labels,webhooks,health,admin}.cpp` | handlers |
| tests | `test_accounts`, `test_api_handlers` (+ `test_dto`) |

---

## B. REST routes → handler → domain calls

Auth: **N** none · **U** user · **A** admin · **S** signed URL · **W** Svix. Body: **–** none
(≤ 4 KiB discarded for POST/DELETE, 0 for GET) · **J** JSON 1 MiB · **D** JSON 8 MiB · **R** raw 1 MiB ·
**F** file 25 MiB. Pool: **db** / **net** (synchronous Resend) / **files** (R2/disk file I/O, §H 15).
All rows are in `api::route_table()` (56 routes, asserted by `test_contracts_compile`). "R:" = inside `svc.db.read`, "W:" = inside one `svc.db.write`, "→net" / "→files" =
network or blob I/O outside any transaction, "after:" = after COMMIT.

| Method | Path | Auth/Body/Pool | Handler (file) | Calls |
|---|---|---|---|---|
| POST | /api/auth/login | N/J/db | `auth_login` (auth.cpp) | `parse_login`; `LoginThrottle::check` → 429 `too_many_attempts{retry_after}`; R: `repo::find_user_by_email`; `ScryptPermit` + `crypto::password_verify` (dummy verify for unknown emails); fail → `record_failure`, W: `repo::audit("login.failure")`, 401 `invalid_credentials`; disabled → 403 `account_disabled`; W: `repo::create_session(ttl=cfg.session_ttl_days)`, `repo::record_login`, `repo::audit`; R: `repo::get_settings`, `repo::identities_for_user`; `api::me_json(server_info(svc))`, `login_response` |
| POST | /api/auth/logout | U/–/db | `auth_logout` | W: `repo::revoke_session(principal.session_id)`; after: `notifier.revoke_session` → 204 |
| GET | /api/auth/me | U/–/db | `auth_me` | R: `repo::get_user`, `get_settings`, `identities_for_user` → `me_json` |
| POST | /api/auth/password | U/J/db | `auth_change_password` | `parse_password_change`, `validate_new_password` (422 `weak_password`); verify current under `ScryptPermit` → 403 `invalid_credentials`; hash new (outside tx); W: `repo::update_user{password_hash}`, `revoke_all_sessions(user, except=current)`, `audit`; after: `notifier.revoke_session(each)` → 204 |
| GET | /api/settings | U/–/db | `settings_get` (settings.cpp) | R: `repo::get_settings` → `to_json(UserSettings)` |
| PUT | /api/settings | U/J/db | `settings_update` | `parse_settings_patch`; W: `repo::update_settings` (emits `settings.changed`) → full Settings |
| GET | /api/identities | U/–/db | `identities_list` | R: `repo::identities_for_user` → `Identity[]` |
| GET | /api/threads | U/–/db | `threads_list` (mail.cpp) | query `folder` (`mail::parse_folder`, else 400 `invalid_field{folder}`) \| `label_id` \| `q`, `cursor`, `limit` (default `repo::get_settings().page_size`), `tzoff`; R: `mail::list_threads` → `to_json(ThreadPage)` |
| GET | /api/threads/:id | U/–/db | `threads_get` | R: `mail::get_thread` → 404 `not_found` |
| POST | /api/threads/actions | U/J/db | `threads_actions` | `mail::parse_thread_action_request`; W: `mail::apply_thread_action` → `thread_ids_response` |
| GET | /api/messages/:id | U/–/db | `messages_get` | R: `mail::get_message` → 404 |
| PATCH | /api/messages/:id | U/J/db | `messages_patch` | `mail::parse_message_patch`; W: `mail::patch_message`; R: `get_message` |
| GET | /api/messages/:id/events | U/–/db | `messages_events` | R: `mail::message_events` → 404 / `events_response` |
| GET | /api/messages/:id/raw | U/–/files | `messages_raw` | R: `mail::find_raw` → 404; →files `svc.blobs_for(storage)`: a `LocalFile` from `serve()` is streamed as-is, otherwise `get_to_file` into `tmp_dir()` → `Response::file(.., "text/plain; charset=utf-8", "")` with `remove_after_send` — never a redirect (the SPA fetches it with a Bearer header; R2 has no CORS) |
| POST | /api/messages/:id/undo-send | U/–/db | `messages_undo_send` | W: `mail::undo_send` (409 `too_late`) → `draft_response` |
| POST | /api/messages/:id/cancel-schedule | U/–/net | `messages_cancel_schedule` | W: `mail::begin_cancel_schedule` (409 `invalid_state` while the scheduling POST is in flight, 409 `already_sent` once sending/sent); if `remote`: →net `resend_client().cancel(resend_id)` (Validation → 409 `already_sent`, other `resend::Error` → 502 `resend_error`); W: `mail::finish_cancel_schedule` → `draft_response` |
| POST | /api/messages/:id/reschedule | U/J/net | `messages_reschedule` | `mail::parse_reschedule`; W: `mail::begin_reschedule(cfg, now)` (422 `invalid_schedule`); if `remote`: →net `update_schedule(resend_id, iso8601_utc(at))` (Validation → 409 `already_sent`, other → 502 `resend_error`); W: `mail::finish_reschedule`; R: `get_message` |
| POST | /api/messages/:id/retry | U/–/db | `messages_retry` | W: `mail::retry_failed_send` (409 `invalid_state`) → 202 `SendResult` |
| GET | /api/counts | U/–/db | `counts_get` | R: `mail::counts` |
| GET | /api/contacts | U/–/db | `contacts_search` | `q`, `limit` (default 8, ≤ 50); R: `mail::search_contacts` → `contacts_response` |
| POST | /api/drafts | U/D/db | `drafts_create` (drafts.cpp) | `mail::parse_draft_input`; W: `mail::create_draft` → 201 Draft |
| GET | /api/drafts/:id | U/–/db | `drafts_get` | R: `mail::get_draft` → 404 |
| PUT | /api/drafts/:id | U/D/db | `drafts_update` | `mail::parse_draft_update`; W: `mail::update_draft` (409 `version_conflict{current}`) |
| DELETE | /api/drafts/:id | U/–/db | `drafts_delete` | W: `mail::delete_draft` → 204 |
| POST | /api/drafts/:id/send | U/D/db | `drafts_send` | `mail::parse_send_options` (`now_ms = svc.now_ms()`); W: `mail::queue_send(cfg, svc.signed_urls, …)` (the overload whose 409 carries `details.current` as a full Draft) → 202 SendResult (403 `send_as_forbidden`; 422 `unknown_local_recipient`/`too_many_recipients`/`invalid_schedule`/`no_recipients`; 413 `message_too_large`; 409 `version_conflict`) |
| POST | /api/attachments | U/F/files | `attachments_upload` (attachments.cpp) | session pre-authenticates before staging the body (§H 24); query `filename` (required, decoded), `inline=0\|1`, Content-Type; `auto g = blob_writer_guard()` held until after the write below; →files `svc.blobs.put_file(req.body_file, req.body_sha256)` (`BlobError` → 503 `storage_unavailable` if retryable else 502 `storage_error`); W: `mail::create_upload` → 201 Attachment. The session deletes `req.body_file` if it is still there (400s, errors) |
| GET | /api/files/:id | S/–/files | `files_get` | query `d,u,exp,sig`; `signed_urls.verify_file` → 403 `invalid_signature`; R: `repo::get_user(u)` active (else 403 `invalid_signature`), `mail::find_attachment(u, id)` → 404; `mail::file_serve_policy`; →files `svc.blobs_for(storage).serve(sha, {ct, disposition, cfg.r2_presign_ttl_sec})`: `LocalFile` → `Response::file` (+ `Content-Security-Policy: sandbox` when `sandbox_csp`, `Cache-Control: private, max-age=3600`), `RedirectUrl` → 302 + `Cache-Control: private, max-age=60`; `BlobNotFound` → 404 |
| GET | /api/files/raw/:messageId | S/–/files | `files_raw` | `verify_raw`; user active; R: `mail::find_raw(u, id)` → 404; serve as `message/rfc822` attachment named `RawRef::filename` |
| GET | /api/labels | U/–/db | `labels_list` (labels.cpp) | R: `repo::list_labels` → `Label[]` |
| POST | /api/labels | U/J/db | `labels_create` | `parse_label_input`; W: `repo::create_label` (409 `label_exists`) → 201 |
| GET | /api/labels/:id | U/–/db | `labels_get` | R: `repo::get_label` → 404 |
| PATCH | /api/labels/:id | U/J/db | `labels_update` | `parse_label_patch`; W: `repo::update_label` |
| DELETE | /api/labels/:id | U/–/db | `labels_delete` | W: `repo::delete_label` → 204 |
| POST | /api/webhooks/resend | W/R/db | `webhooks_resend` (webhooks.cpp) | headers `svix-id`, `svix-timestamp`, `svix-signature`; `resend::verify_svix(cfg.resend_webhook_secret, …, now_s, cfg.webhook_tolerance_sec)` ≠ Ok → 401 `invalid_signature`; `resend::parse_webhook` (invalid → 400 `invalid_json`); W: `jobs::process_webhook(tx, env, svix_id, body, now)` → 200 `{ok:true}` |
| GET | /api/health | N/–/db | `health_get` (health.cpp) | R: `SELECT 1` → `health_json("ok", AZMAIL_VERSION, "ok", now)`; DB failure → 503 with `db:"error"` |
| GET | /api/admin/users | A/–/db | `admin_users_list` (admin.cpp) | R: `repo::list_users_admin` |
| POST | /api/admin/users | A/J/db | `admin_users_create` | `parse_admin_user_create`, `validate_new_password`; hash (outside tx); W: `repo::create_user` (`NewUser::undo_send_seconds = cfg.default_undo_send_seconds`; 409 `address_exists`, 422 `unknown_domain`), `audit`; R: `get_user_admin` → 201 |
| PATCH | /api/admin/users/:id | A/J/db | `admin_users_update` | `parse_admin_user_patch`; hash password if present; W: `repo::update_user` (409 `last_admin`), `revoke_all_sessions` when password/disabled changes, `audit`; after: `notifier.revoke_user` (disabled) / `revoke_session` (password) |
| DELETE | /api/admin/users/:id | A/–/db | `admin_users_delete` | W: `repo::delete_user(id, principal)` (409 `cannot_delete_self`/`last_admin`), `audit`; after: `notifier.revoke_user` → 204 |
| GET | /api/admin/aliases | A/–/db | `admin_aliases_list` | R: `repo::list_aliases` |
| POST | /api/admin/aliases | A/J/db | `admin_aliases_create` | `parse_alias_input`; W: `repo::create_alias`, `audit` → 201 |
| PATCH | /api/admin/aliases/:id | A/J/db | `admin_aliases_update` | `parse_alias_patch`; W: `repo::update_alias` (members replaced wholesale), `audit` |
| DELETE | /api/admin/aliases/:id | A/–/db | `admin_aliases_delete` | W: `repo::delete_alias` (404; 409 `alias_in_use` when outbound rows reference it), `audit` → 204 |
| GET | /api/admin/domains | A/–/db | `admin_domains_list` | R: `repo::list_domains` |
| POST | /api/admin/domains | A/J/db | `admin_domains_create` | `parse_domain_input`; W: `repo::add_domain` (409 `domain_exists`), `audit` → 201 |
| GET | /api/admin/domains/:id | A/–/db | `admin_domains_get` | R: `repo::get_domain` → 404 |
| DELETE | /api/admin/domains/:id | A/–/db | `admin_domains_delete` | W: `repo::remove_domain` (409 `domain_in_use`), `audit` → 204 |
| GET | /api/admin/domains/:id/status | A/–/net | `admin_domains_status` | R: `repo::get_domain` → 404; →net `resend_client().list_domains()` (match by name) → `get_domain(resend id)`; `domain_status_json` (`resend:null` when unknown; `records[].priority` omitted when absent, matching `priority?: number`); `resend::Error` → 502 `resend_error` |
| GET | /api/admin/events | A/–/db | `admin_events_list` | `type`, `cursor`; R: `repo::list_webhook_events(type, cursor, 50)` → CursorPage |
| GET | /api/admin/inbound | A/–/db | `admin_inbound_list` | `state`; R: `repo::list_inbound(state, 200)` |
| GET | /api/admin/outbox | A/–/db | `admin_outbox_list` | `status`; R: `repo::list_outbox(status, 200)` |
| POST | /api/admin/outbox/:id/retry | A/–/db | `admin_outbox_retry` | W: `mail::admin_retry_outbound` (409 `invalid_state`); R: `repo::get_outbox_row(new id)` → the **NEW** OutboxRow (new id and uuid); the original row stays `failed`. WP-F invalidates `['admin','outbox']` instead of patching by id |
| GET | /api/admin/jobs | A/–/db | `admin_jobs_list` | `state`; R: `repo::list_jobs(state, 200)` |
| POST | /api/admin/jobs/:id/retry | A/–/db | `admin_jobs_retry` | W: `jobs::retry_dead` (false → 409 `invalid_state`); R: `repo::get_job` → JobRow |
| POST | /api/admin/sync | A/–/db | `admin_sync` | W: `jobs::enqueue(poll.receiving, {manual:true}, dedupe "poll:manual")` → 202 `{job_id}` |
| GET | /api/admin/stats | A/–/db | `admin_stats_get` | R: `repo::admin_stats`; `storage.backend = svc.blobs.kind()`, `storage.delivery = enum_name(svc.cfg.files_delivery)` (App's config, after the presign-probe fallback) |
| GET | /api/ws | WS | (not a route) | `http::run_session` → `ws::run_ws_session` → `http::authenticate` (first message, then every 5 min: nullopt → `session.revoked` + close 4401) |

Every handler not listed with explicit codes can also throw 400 `invalid_json` / `invalid_field`,
401 `unauthorized` (dispatch), 403 `forbidden` (dispatch, admin), 404 `not_found`.

---

## C. Job kinds → handler → functions

All handlers are declared in `jobs/handlers.hpp` and registered by `register_all_jobs(Runner&)`.
Common rules (§H 28–30): times from `svc.clock`; `jobs::EnqueueOpts::now_ms` passed when enqueuing;
on the last attempt any attempt-consuming failure records the terminal domain state
(`mark_failed` / `mark_inbound_failed`) before rethrowing; long transfers call `jobs::extend_lease`;
periodic kinds always get a successor when they end (done **or dead**); every handler runs under
`resend::ScopedStopToken` so Resend calls stop blocking on shutdown.

| Kind (lane) | Handler (file) | Calls |
|---|---|---|
| `outbound.send` (outbound, prio 100, max 10 attempts) | `run_outbound_send` (outbound_jobs.cpp) | W: `mail::mark_sending` (false → done); R: `mail::load_send_plan`; parent id missing → `resend::Client::get(parent_resend_id)` → W: `mail::set_outbound_message_id` → reload; `svc.blobs_for(storage).get_bytes` + `crypto::b64_encode`; `resend::Client::send`; W: `mail::mark_accepted` + `jobs::enqueue(outbound.fetch_meta)`; errors → `mail::note_send_retry` / `mail::mark_failed` / `mail::switch_to_local_schedule`, `db::kv_set(kQuotaBlocked)`, `Retry` / `Permanent`; last attempt: any error → `mark_failed`; backoff `outbound_backoff` |
| `outbound.fetch_meta` (sync) | `run_outbound_fetch_meta` | R: `mail::get_outbound`; `Client::get(resend_id, Low)`; W: `mail::set_outbound_message_id`, `mail::apply_outbound_event(source_key "poll:<last_event>")` |
| `outbound.reconcile` (maintenance, periodic `cfg.reconcile_interval_sec`) | `run_outbound_reconcile` | R: `mail::outbound_to_reconcile(now, 7 d, 50)`; `Client::get`; W: `mail::apply_outbound_event` |
| `inbound.fetch` (inbound, prio 50, dedupe `in:<id>`) | `run_inbound_fetch` (inbound_jobs.cpp) | `Client::get_received`; `Client::download_to(raw)` into tmp; `eml::read_file_prefix` + `eml::parse_headers`; `Client::list_received_attachments` → `download_to` into tmp (`jobs::extend_lease` before each); then ONE `blob_writer_guard()` around every `BlobStore::put_file` + W: `mail::deliver_inbound(build_inbound_email(…))`; permanent failure or last attempt W: `mail::mark_inbound_failed` |
| `poll.receiving` (sync, periodic `cfg.poll_interval_sec`; manual dedupe `poll:manual`) | `run_poll_receiving` | `Client::list_received`; R: `mail::inbound_state`; W: `mail::record_inbound_pending` + `jobs::enqueue(inbound.fetch, source poll)`, `kv last_poll_at / poll.high_water / poll.gap_warning` |
| `purge.trash` (maintenance, hourly) | `run_purge_trash` (maintenance_jobs.cpp) | W: `mail::purge_trash(now, cfg.trash_purge_days, cfg.spam_purge_days, 500)` until `!more` |
| `gc.blobs` (maintenance, hourly) | `run_gc_blobs` | R: `mail::unreferenced_blobs(now − cfg.blob_gc_grace_hours)`; per blob under its own `blob_gc_guard()`: R: `mail::is_blob_unreferenced` → `svc.blobs_for(storage).remove` (outside any tx; `BlobError` → log, keep the row, retried next run) → W: `mail::forget_blob_if_unreferenced` (DESIGN Addendum A order "remove() → delete row") |
| `gc.housekeeping` (maintenance, hourly) | `run_gc_housekeeping` | W: `repo::purge_expired_sessions`, `jobs::purge_finished`, `DELETE FROM webhook_events` (retention), `mail::purge_orphan_uploads`, `jobs::recover_expired_leases`; then deletes files older than 24 h in `tmp_dir()` of every configured store |
| `db.optimize` (maintenance, daily) | `run_db_optimize` | `PRAGMA optimize; PRAGMA wal_checkpoint(TRUNCATE)` |

Enqueued by: `outbound.send` ← `mail::queue_send`, `mail::retry_failed_send`, `mail::admin_retry_outbound`;
`outbound.fetch_meta` ← `run_outbound_send`, `mail::finish_reschedule` (moves it), `mail::apply_outbound_event`
(uuid match with a lost POST response, §H 31); `inbound.fetch` ←
`jobs::process_webhook`, `run_poll_receiving`; `poll.receiving` ← Runner (periodic), `admin_sync`;
the rest ← Runner (periodic). Webhooks are not jobs: `jobs::process_webhook` runs inside the request.

---

## D. WebSocket events → emitter

Payloads are built only with `ws/events.hpp` builders and published only via `tx.emit` (except
`ready`/`pong`, written by the WS session itself, and `session.revoked`, written by the Hub).

| Event | Emitted by |
|---|---|
| `ready`, `pong` | `ws::run_ws_session` (WP-A), direct |
| `mail.new` | `mail::deliver_inbound` (WP-B), one per new copy and owner |
| `threads.changed` | WP-B: `apply_thread_action`, `patch_message`, `create_draft`, `update_draft`, `delete_draft`, `queue_send`, `undo_send`, `merge_threads`/`adopt_referencing`, `finish_cancel_schedule`, `begin_cancel_schedule` (local), `finish_reschedule`, `retry_failed_send`, `admin_retry_outbound`, `purge_trash`, loopback merge in `deliver_inbound`, late loopback cleanup in `set_outbound_message_id`; WP-D: `repo::delete_label` |
| `outbound.status` | WP-B: `mark_accepted`, `mark_failed`, `apply_outbound_event` (on status change), `switch_to_local_schedule`, `finish_cancel_schedule`, `finish_reschedule` — to every owner holding a copy (C3) |
| `labels.changed` | WP-D: `repo::create_label`, `update_label`, `delete_label` |
| `settings.changed` | WP-D: `repo::update_settings`, `repo::update_user` when `display_name` changes |
| `session.revoked` | WP-A: `ws::Hub::revoke_session` / `revoke_user`, called after COMMIT by WP-D handlers (logout, password change, admin disable/delete); `ws::run_ws_session` itself when the 5-minute re-authentication fails (sessions revoked by another process, e.g. `azmail reset-password`, or expired) |

---

## E. Error codes

Codes in API.md are used verbatim. Codes **added in WP0** are marked ★ (the frontend's
`KnownErrorCode` already allows unknown strings).

| Status | Code | Raised by |
|---|---|---|
| 400 | `bad_request`, `invalid_json`, `invalid_field` `{field}` | core/json getters, parsers, `Ctx::id`, `Ctx::query_int` |
| 401 | `unauthorized` | dispatch (missing/invalid Bearer) |
| 401 | `invalid_credentials` | login |
| 401 | ★`invalid_signature` | webhook Svix failure |
| 403 | `forbidden` | dispatch (non-admin on admin route), CORS preflight |
| 403 | `invalid_credentials` | password change with a wrong current password (frontend Addendum B) |
| 403 | ★`account_disabled` | login of a disabled user (after a correct password) |
| 403 | ★`invalid_signature` | bad/expired signed URL or inactive user (`/api/files/*`) |
| 403 | `send_as_forbidden` | drafts / send |
| 404 | `not_found` | any foreign or missing id (IDOR), unknown route |
| 405 | ★`method_not_allowed` | session (with `Allow`) |
| 409 | `version_conflict` `{current}` | update_draft, queue_send (6-argument overload; `current` is a full Draft) |
| 409 | `too_late` | undo_send |
| 409 | `already_sent` | cancel-schedule, reschedule (send under way at/after `scheduled_at`, `sent` or later, or Resend rejected the cancel/PATCH) |
| 409 | ★`invalid_state` | retry of a non-failed send, reschedule/cancel of a non-scheduled message or while the scheduling POST is in flight (`sending` with `scheduled_at` > now: "正在提交定时发送，请稍后重试"), admin job/outbox retry |
| 409 | ★`address_exists`, ★`domain_exists`, ★`label_exists` | repo creates/updates |
| 409 | ★`last_admin`, ★`cannot_delete_self` | repo::update_user / delete_user |
| 409 | ★`domain_in_use` | repo::remove_domain |
| 409 | ★`alias_in_use` | repo::delete_alias when outbound rows reference the alias (`outbound.from_address_id` has no ON DELETE action) — remove its members instead |
| 413 | `payload_too_large` | session body limits |
| 413 | `message_too_large` | queue_send |
| 422 | `unknown_local_recipient` `{emails}`, `too_many_recipients` `{field}`, `invalid_schedule` | queue_send, begin_reschedule |
| 422 | ★`no_recipients` | queue_send |
| 422 | ★`unknown_domain` | create_user / create_alias (CLI `create-user --create-domain` avoids it) |
| 422 | ★`weak_password` | password change, admin create/patch |
| 429 | `too_many_attempts` `{retry_after}` | login throttle |
| 500 | `internal_error` | dispatch (unexpected exception, incl. `NotImplemented`) |
| 502 | `resend_error` | cancel-schedule, reschedule, domain status |
| 502 | ★`storage_error` | upload / files with a non-retryable `BlobError` |
| 503 | `service_unavailable` | in-flight cap, `db::BusyError`, `Services::resend_client()` when unset |
| 503 | ★`storage_unavailable` | retryable `BlobError` |

---

## F. kv keys (`db/kv.hpp`)

| Key | Writer | Reader |
|---|---|---|
| `last_webhook_at` | `jobs::process_webhook` | `repo::admin_stats` |
| `last_poll_at`, `poll.high_water`, `poll.gap_warning` | `run_poll_receiving` | admin stats / poller |
| `resend.quota_blocked` | `run_outbound_send` (set on quota), cleared by a later successful send | `repo::admin_stats` (`quota_blocked`) |

---

## G. Rules

1. **IDOR**: every mail/domain function takes `owner` explicitly and filters every query on it; foreign
   ids behave like missing ids (404). Functions keyed only by outbound/inbound/job ids are
   system-internal and are never called with client-supplied ids.
2. **No network in transactions**: Resend, R2 and downloads happen outside `Pool::write`; register
   `blobs` rows only after `put_file` succeeded. Write closures must be re-runnable (BUSY retry).
   Blob writers hold `blob_writer_guard()` from `put_file` until the registering + referencing
   transaction committed; `gc.blobs` holds `blob_gc_guard()` per blob (§H 23). Never call
   `BlobStore::remove` from `tx.after_commit`.
3. **WS only via `tx.emit`** with the `ws/events.hpp` builders; `Notifier::revoke_*` only after COMMIT.
4. **Stored HTML uses `cid:`** — `html` and `quoted_html` alike; signed URLs exist only in
   responses (`mail/render.hpp`) and are stripped from outgoing mail (`strip_api_file_urls`, §H 26).
5. **Times** are ms-epoch UTC everywhere; Resend ISO strings go through `parse_iso8601_lenient` /
   `iso8601_utc`. Clock: code with a `Clock`/`now_ms` uses it and passes it on
   (`EnqueueOpts::now_ms`); functions without a `now_ms` parameter use `azm::now_ms()`, so tests
   that mix both run their `ManualClock` at real time (`mail/types.hpp`, §H 30).
6. **Errors** are `ApiError` with the codes in §E; never put user data in `what()`; never log secrets,
   tokens, signed-URL queries or mail bodies.
7. **Account tables** (`users`, `addresses`, `alias_members`, `domains`, `user_settings`, `labels`,
   `sessions`, `audit_log`) are written only by `repo::*` (WP-D). `mail::*` may read them with its own
   SQL and never writes them. Mail tables are written only by `mail::*` (WP-B); `jobs` table only by
   `jobs::*` (WP-C); `webhook_events` by `jobs::process_webhook` and gc.housekeeping; `kv` per §F.
   Exception: `azmail blobs-migrate` (WP-A CLI) flips `blobs.storage` after copying and verifying a blob.
   CLI commands otherwise reuse the owners' functions (`repo::create_user`, `repo::update_user` +
   `revoke_all_sessions`, `repo::add_domain`, `mail::fts_rebuild_all`, `storage::probe_r2`).
8. **Contracts are frozen**: additive changes only, by the owner, announced. Stubs throw
   `NotImplemented`; replace them, don't change signatures.
9. **Tests are hermetic**: temp dirs, temp SQLite, `ManualClock`, `RecordingNotifier`; network only
   against fakes (`net::HttpClient` / `resend::Client` test doubles) or the Python mock fixture.
   Use `tests/unit/test_support.hpp` (`TestServices`, `seed_*`) instead of hand-rolled fixtures;
   its raw-SQL account seeding is the only test-side exception to rule 7.

---

## H. Decisions made during WP0

Smallest additive decisions where DESIGN.md was silent or contradictory (DESIGN.md unchanged):

1. **Routing and send-as enforcement live in `mail` (WP-B)**: `mail::resolve_local_recipients`,
   `unknown_local_recipients`, `resolve_sender`, `default_from_address`. Rationale: they are mail
   semantics (C1/C3/C4) and WP-B must test delivery without WP-D. `repo::identities_for_user` and
   `repo::can_send_as` (WP-D, for Me/identities) implement the **same rule**: own mailbox address, or an
   alias with `alias_members.can_send_as=1` (documented in `mail/drafts.hpp`); E2E 12 checks it.
2. **New files**: `mail/attachments.*` (B), `jobs/handlers.hpp` (C), `api/handlers.hpp` (D),
   `ws/events.hpp` (A, frozen by WP0), `db/kv.hpp` (WP0), `http/types.cpp` (A, implemented in WP0),
   `app/config.cpp` (A), `resend/types.cpp` (C).
3. `http::Router` is declared in `http/router.hpp` (DESIGN §3 showed it inside the `http/types.hpp` block).
4. `http::Request` gains `query` (decoded params, first wins, `+` is not a space), `body_sha256`
   (File mode, Addendum A), `header()`, `Request::make()`; `FileRef` gains `remove_after_send`;
   `Response` gains `from_error/text/redirect/add_header/find_header`; `Ctx` gains `query_int`.
5. `net::HttpClient::send` and all `resend::Client` methods are **virtual** with protected default
   constructors, so tests can use fakes; call syntax is unchanged. Resend DTOs live in
   `resend/types.hpp` (included by `client.hpp`, so §3 names are unchanged). `resend::Client` adds
   `download_to` (sha256 while streaming), `list_domains`, `get_domain`.
6. `Services`: references for cfg/db/blobs/signed_urls/notifier/clock; nullable pointers for resend,
   http, rate_limiter, runner, login_throttle, secondary_blobs (mixed store during `blobs-migrate`),
   db_workers, net_workers; helpers `now_ms()`, `blobs_for(storage)`, `resend_client()` (→ 503).
7. `recompute_thread(tx, owner, thread_id)` takes `owner` (IDOR rule). `assign_thread` takes
   `ThreadingKeys` and runs **before** the message insert (`messages.thread_id` is NOT NULL);
   `adopt_referencing` handles the reverse direction (B2).
8. `begin_cancel_schedule` returns a `CancelPlan` (local cancels finish immediately);
   `finish_cancel_schedule(tx, urls, owner, outbound_id)` returns the `Draft` (§3 had
   `void … // → draft`). Reschedule mirrors it (`begin_reschedule` / `finish_reschedule`).
9. `mark_sending` is `queued|sending → sending`, so retries re-enter; undo stays `queued`-only.
10. Resend scheduling rejection: `switch_to_local_schedule` + `Retry{scheduled_at − now,
    count_attempt=false}` instead of a re-enqueue (the running job holds the dedupe key).
11. `jobs::enqueue` derives the lane from the kind (`kinds::lane_for`); `outbound.send` uses
    `max_attempts = 10` (1 + the 9-step backoff ≈ 15.4 h < 24 h idempotency window).
    `Retry`/`Permanent` gained constructors; `Job` gained `lane`, `priority`; `Runner` gained
    `run_one(lane)` (tests) and `services()`; register functions take only `Runner&`.
12. `jobs::enqueue`, `jobs::cancel` and `jobs::reschedule` are **implemented in WP0** (plain SQL, tested
    in `test_contracts_compile`) because WP-B's `queue_send` / `undo_send` / reschedule call them and
    DESIGN §7 lists them as a WP0-header dependency. WP-C owns `jobs.cpp` and may refine them
    without changing semantics. Everything else in `jobs.cpp` is a stub.
13. Manual admin sync enqueues `poll.receiving` with dedupe `poll:manual`; periodic jobs use
    `periodic:<kind>`.
14. `api::route_table()` is part of the contract and already filled in; `register_routes(Router&,
    const Config&)` overload applies config body limits. Bodyless POST/DELETE routes accept and
    discard ≤ 4 KiB (clients that send `{}` keep working).
15. **Pools** (revised by the WP0 contract fix): `/api/files/:id`, `/api/files/raw/:id`,
    `/api/messages/:id/raw` and `POST /api/attachments` run on a dedicated **files** pool
    (`http::Exec::Files`, `cfg.files_threads = 8`, `ServerDeps::files_pool`,
    `Services::files_workers`) because proxy-mode cache misses and uploads are synchronous R2
    transfers (seconds across the border); the **net** pool (`cfg.net_threads = 2`) keeps only the
    synchronous Resend calls (cancel-schedule, reschedule, domain status). `cfg.db_pool_size` is 24
    (≥ 8 db + 2 net + 8 files + 6 job threads). `ServerDeps::files_pool` is a pointer (null → net
    pool) so existing aggregate initializers stay valid.
16. WS: `session.revoked` frame then close 4401; attaching beyond `cfg.ws_max_sessions_per_user`
    closes that user's oldest socket with 1008. The `Hub` stub's publish/revoke are no-ops so pool
    hooks are harmless before WP-A lands.
17. Counts (`GET /api/counts`) are **thread** counts, including per-label `{unread,total}`.
18. Error codes marked ★ in §E. `jobs::WebhookResult` adds `duplicate` (svix_id replay).
19. `inbound_emails` rows are created `pending` by `mail::record_inbound_pending` (webhook / poller);
    `deliver_inbound` upserts metadata and registers the raw + attachment blob rows itself.
20. `Draft.mode` is always serialized (default `new`), matching the frontend's API.md Addendum B notes.
21. **iconv**: `CMakeLists.txt` now links `Iconv::Iconv` into `azmail_lib` (macOS needs `-liconv` for
    `mail/eml.cpp`; glibc has it built in).
22. `Config::files_delivery` defaults to **`Proxy`** (Addendum A), like `storage::R2Options::delivery`
    (asserted in `test_contracts_compile`). App switches its own config copy to `Proxy` when the
    presign-override probe fails in redirect mode (§H 34), and admin stats report that value.

### WP0 contract fix (adversarial review) — decisions 23–37

23. **Blob GC vs. writers** (`core/blob_store.hpp`): `put_file`/`put_bytes` skip uploads of existing
    objects, so GC could delete an object a writer just "stored". New process-wide guards
    `blob_writer_guard()` (shared) / `blob_gc_guard()` (exclusive), implemented in WP0. Writers:
    `api::attachments_upload` (put_file → `create_upload` commit), `jobs::run_inbound_fetch` (one
    guard around all of an email's `put_file`s + the deliver tx; downloads happen before it),
    `blobs-migrate`. `gc.blobs` per blob: re-check (`mail::is_blob_unreferenced`, new) →
    `remove()` outside any tx → `forget_blob_if_unreferenced`. This is the DESIGN Addendum A order
    ("remove() → delete row"): a failed remove keeps the row and is retried; nothing runs in
    `after_commit`. `R2BlobStore::remove` evicts its own FileCache entry.
24. **Upload temp files**: the session deletes `req.body_file` after the handler returns or throws
    and on every early exit (body read failures, 503, dispatch errors); `FileRef::remove_after_send`
    files are removed even when the write fails. File routes are pre-authenticated on the db pool
    before the body is read (401/403 with `Connection: close`, nothing staged). `gc.housekeeping`
    and `App::start` delete files older than 24 h in each store's `tmp_dir()`.
25. **`queue_send` overload with `SignedUrls`**: `queue_send(tx, cfg, urls, owner, draft_id, opts)`
    is what `drafts_send` calls, so 409 `version_conflict` carries `details.current` as a full
    Draft (signed URLs, cid → URL rewritten). The 5-argument form stays (source compatibility) and
    its 409 has `details: {}`.
26. **Quoted images / signed-URL leak**: reply/reply_all/forward drafts copy the parent's inline
    attachments (same blob and `content_id`; forward + `include_parent_attachments` copies all);
    `create_draft`/`update_draft`/`queue_send` run `rewrite_signed_to_cid` on `quoted_html` too,
    with targets = draft attachments ∪ parent inline attachments (parent id → same content_id);
    `Draft.quoted_html` is returned with signed URLs like `html`. At freeze,
    `mail::strip_api_file_urls(html, cfg.public_api_base_url)` (new) runs after `strip_att_ids`,
    and (except in forward mode) unreferenced inline attachments are dropped from the message.
27. **Loopback without `X-AzMail-Ref`** (E2E 08): `deliver_inbound` also merges when
    `email.message_id` equals the `message_id_header` of an outbound the owner holds a copy of;
    `set_outbound_message_id` deletes an earlier-inserted `in` copy with that Message-ID (read/star/
    labels OR-ed into the out copy, `in_inbox=1`, threads recomputed, `threads.changed`).
28. **Job robustness**: periodic kinds enqueue their successor whenever they end, done or dead;
    on the last attempt handlers record `mark_failed` / `mark_inbound_failed` before rethrowing;
    `jobs::extend_lease(tx, job_id, attempts, locked_until)` (implemented in WP0) renews leases for
    long transfers and fails for stale claims; Runner outcomes are conditional on the claim
    (`attempts`), so a stale worker changes nothing.
29. **Shutdown and the rate limiter**: `resend::ScopedStopToken` / `current_stop_token()` (inline,
    `resend/rate_limiter.hpp`): Runner installs the job's token; `resend::Client` passes it to
    `RateLimiter::acquire` and throws `Error{Kind::Network, "stopped"}` when stopped.
30. **Clock injection**: `EnqueueOpts::now_ms` (0 → real time; implemented); Runner compares
    `run_at` with `services().clock`; mail functions without a `now_ms` parameter use real time
    (`mail/types.hpp`), so `TestServices` starts its `ManualClock` at real time.
31. **Lost POST response (B4)**: `apply_outbound_event` matched by uuid stores `ev.resend_id` when
    `outbound.resend_id` is NULL and enqueues `outbound.fetch_meta` (dedupe `out:meta:<id>`) when
    the Message-ID is still unknown.
32. **Config that now takes effect**: `SignedUrls(secret, base, ttl_ms)` + `ttl_ms()` +
    `expiry(now)` (hour rounding only for TTL ≥ 1 h; implemented, tested) — App passes
    `cfg.signed_url_ttl_sec`, mail rendering uses `urls.expiry(now)`; `repo::NewUser::
    undo_send_seconds` (CLI and admin create pass `cfg.default_undo_send_seconds`);
    `Config::fts_body_limit` is documented as informational (fixed `mail::kFtsBodyLimit`, no env).
33. **Test fixtures** (`tests/unit/test_support.hpp`, WP0): `migrate`, `seed_domain` (get-or-create),
    `seed_user` (users + address + settings, domain auto-created), `seed_alias`, `seed_session`
    (raw token; hash stored as BLOB like `repo::create_session`), `address_id`, and `TestServices`
    (TempDir, migrated Pool with TxHooks → RecordingNotifier, LocalBlobStore, SignedUrls,
    ManualClock at real time, Config, Services).
34. **App wiring**: order Pool → HttpClient → `probe_r2` (presign check only in redirect mode;
    failure → own config `files_delivery = Proxy`) → BlobStores → … (`app/app.hpp`); App always
    wires `Services::secondary_blobs` (local when backend = r2; r2 when backend = local and R2 is
    configured), and `Services::blobs_for` now throws `BlobError{retryable=false}` when no store
    matches instead of silently using the primary.
35. **WS re-authentication**: every 5 min `ws::run_ws_session` re-runs `http::authenticate`;
    nullopt → `session.revoked` + close 4401 (covers revocations by other processes and expiry).
36. **Admin/API shapes**: `POST /api/admin/outbox/:id/retry` returns the NEW OutboxRow (new id and
    uuid; the old row stays `failed`); `DomainStatus` omits an absent `records[].priority`
    (frontend `priority?: number`); `repo::delete_alias` raises ★`alias_in_use` (the schema cannot
    use ON DELETE SET NULL on the NOT NULL column); cancel/reschedule while the scheduling POST is
    in flight raise 409 `invalid_state`, `already_sent` only once the send is under way or done.
37. **CLI / E2E bootstrap** (notes for WP-G): `azmail create-user … [--create-domain]` creates the
    missing domain first. E2E 01 runs `add-domain` (or `--create-domain`) before `create-user`, and
    E2E 02 must not re-create that domain (409 `domain_exists`) — it creates only additional ones.
    The WS test client (`tests/e2e/lib/ws.py`) must send an `Origin` taken from
    `AZMAIL_CORS_ORIGINS`: a missing Origin is rejected (`http/cors.hpp`).

**Frontend follow-ups** (frontend files are outside this backend fix; for their owners):
`frontend/src/api/types.ts` `KnownErrorCode` should list `alias_in_use` (409) — unknown codes
already type-check; the `POST /api/admin/outbox/:id/retry` comments in `types.ts` and
`api/admin.ts` should say "returns the NEW OutboxRow (new id and uuid)", and WP-F should invalidate
`['admin','outbox']`; `DomainDnsRecord.priority?: number` stays as is (backend omits it).

---

## I. WP-B1 decisions (mail domain, read side)

Additive notes where DESIGN/headers left room; WP-B2 continues from this branch.

1. **New internal files** (not contracts): `mail/internal.{hpp,cpp}` (address-list JSON columns
   `[{name,email}]`, LIKE escaping, `json_each` id lists, owner address set, MIME normalization)
   and `mail/html_scan.{hpp,cpp}` (forgiving HTML tokenizer with attribute spans + entity
   decoding, shared by `html_text` and `render`). WP-B2 writes `messages.*_json` with
   `detail::addresses_to_json`. Test fixtures for mail rows: `tests/unit/mail_fixtures.hpp`.
2. **`normalize_subject` keeps leading list tags** (`"[team] 周报"`), removing reply/forward
   prefixes around them (`"Re: [ops] Re: 周报"` → `"[ops] 周报"`), per the WP-B1 brief; the WP0
   header comment said tags were removed and was updated. ASCII lowercase, whitespace collapsed.
   `has_reply_prefix` also looks past leading tags.
3. **Threading**: `assign_thread` looks up the message's own id together with its refs, so a reply
   that arrived first is found directly (merge into the lowest thread id when several match);
   `adopt_referencing` covers ids learned later. Subject fallback participants exclude the owner's
   own addresses (mailbox + aliases), otherwise every thread would "overlap"; candidates need
   `|last_at − date| ≤ 7 d` (threads with only trash/spam have `last_at = 0` and never match).
4. **Message sets**: spam = `is_spam=1 AND trashed_at IS NULL`, trash = `trashed_at IS NOT NULL`
   (disjoint). `recompute_thread`: drafts never count as unread; snippet/last_message_id fall back
   to the latest draft, then the latest message of any set, so drafts-only / spam / trash threads
   still show text; participants are senders of non-draft messages (drafts' author when there are
   only drafts), first + 5 most recently active when > 6; `attachment_count` = non-inline
   attachments of non-draft messages; `subject` = earliest non-draft message with a subject.
5. **Thread list**: keyset cursor = base64url(`"<sort_key>:<thread_id>"`); folder queries spell
   the partial-index predicates verbatim (asserted with EXPLAIN QUERY PLAN). Per-view scope:
   spam/trash views use `spam_last_at` / `trash_last_at`, spam/trash unread and labels/previews of
   that set; other views the normal set. Search orders threads by the latest *matching* message
   date (also returned as `last_at`); `in:spam`/`in:trash`/`in:anywhere` switch the scope.
   `latest_status` = status of the latest non-draft message with an outbound row in the view;
   `scheduled_at` = earliest `scheduled_at` with the `scheduled_count` status rule.
6. **Thread actions**: `inbox` = move to inbox (clears spam + trash, `in_inbox=1` on received
   non-draft messages, or on sent ones when the thread has none received); `read`/`unread`/`spam`
   skip drafts; `star` stars the latest normal non-draft message; `unstar` and `remove_label`
   apply to all messages; `add_label` to normal messages; `delete_forever` deletes only trashed or
   spam messages. Each affected thread is recomputed; one `threads.changed` per call.
7. **Search**: invalid operator values (`in:foo`, `after:yesterday`) fall back to literal text;
   `cc:` / `bcc:` match `messages.cc_json` / `bcc_json` via `json_each` (`to:` = FTS `to_text`,
   i.e. to + cc (+ own bcc)); `label:a-b` also matches the label "a b"; `has:attachments` is
   accepted; `larger:` is `>=`, `smaller:` is `<`; `before:` is exclusive of that local day.
   Short-term LIKE runs on the message's own FTS row (`f.rowid = m.id`) so the scan stays within
   the owner's candidate messages.
8. **`html_to_text` link format** follows the frozen header: `text <url>` (the brief said
   `text (url)`); links whose text equals the URL / mailto address, image-only links and `#`,
   `javascript:`, `cid:`, `data:` targets add nothing. `make_snippet` keeps the header default of
   200 code points and falls back to the quoted text when the body is only a quote.
9. **`purge_trash`**: a negative day count disables that half, 0 purges immediately.
10. **`attachments.hpp` gained `thread_attachments(conn, owner, thread_id)`** (additive).
11. **WP0 stub tests**: `test_contracts_compile.cpp` "WP0 stubs throw NotImplemented" (lines with
   `mail::list_threads`, `mail::compile_search`) and "WP0 fixer stubs link"
   (`mail::is_blob_unreferenced`, `mail::strip_api_file_urls`) assert that WP-B functions are
   still stubs and fail once they are implemented; WP0 (the owner) must drop those assertions.

---

## J. WP-B2 decisions (mail domain, write side)

Additive notes for drafts, outbound and inbound where DESIGN/headers left room.

1. **New internal files** (not contracts): `mail/send_internal.{hpp,cpp}` — the frozen payload
   format, delivery-event bookkeeping, status fan-out to every copy, shared-copy removal, the
   "back to draft" transition, the send-job enqueue. Test fixtures: `tests/unit/send_fixtures.hpp`.
2. **Drafts are `direction='out', is_draft=1`**, `is_read=1`, `in_inbox=0`; `date`/`updated_at`
   move on every save (so the thread's `last_at` follows the latest edit, §2). Subjects are stored
   with CR/LF/TAB replaced by spaces.
3. **`outbound.payload_json`** (internal, read only by `load_send_plan`): formatted from/to/cc/bcc,
   subject, frozen html/text, `attachment_ids`, `parent_message_id`, the parent Message-ID known at
   freeze (`in_reply_to`), the parent's References chain without the parent id (`references`) and
   the pre-freeze draft body (`draft.html`, `draft.quoted_html`) that undo / cancel restore. The
   parent id is re-resolved at load time (frozen → parent message row → parent outbound row), and
   a reply to our own sent mail reuses that mail's resolved chain. Inbound References are kept in
   order in `inbound_emails.meta_json.references` (message_refs is an unordered set); meta_json
   holds header metadata only, never bodies.
4. **Freeze**: `<div style="font…">body + <div class="azm-signature">signature</div> +
   <div class="gmail_quote azm-quote">quoted_html</div></div>`, then `strip_att_ids` and
   `strip_api_file_urls`. The signature is skipped when disabled/empty or when the body's text
   already contains the signature's text (the editor inserted it); image-only signatures are
   always appended. `gmail_quote` keeps the quote out of snippets. In-Reply-To/References are set
   for reply, reply_all **and forward** (Gmail-like threading); recipients are de-duplicated
   across To → Cc → Bcc (case-insensitive) in the payload and the stored copy.
5. **Recipient validation**: `unknown_local_recipients` also lists local addresses that cannot
   receive (disabled user, alias without active members), since that mail would be silently lost
   (C4). `no_recipients` is checked before `too_many_recipients` and the local-recipient check;
   `message_too_large` details: `{attachment_id, limit}` or `{total_bytes, limit}`.
6. **Attachments on drafts**: `attachment_ids` is the full list on update, but inline attachments
   still referenced by html/quoted_html (by id, `cid:`, or through the parent attachment a quote
   URL names) are kept; on create the list only adds uploads (the copied quote images are new to
   the client). Owner's unattached **inline** uploads referenced by `data-att-id` / file URL in the
   client HTML are attached automatically (E3 paste flow). Copied parent attachments: Content-ID
   set and (inline or referenced by the parent's HTML) → copied as inline; forward +
   `include_parent_attachments` copies everything as is.
7. **Reply defaults** when a create request omits them: subject `Re: …` / `Fwd: …` (kept when
   already prefixed); reply → `reply_to` or `from` (to our own sent mail: its To); reply_all →
   that + To, Cc = parent Cc, minus the owner's own addresses (falls back to From when nothing is
   left); forward → no recipients. From defaults (reply/reply_all only) to the parent's
   `delivered_to` or, for our own sent parent, its `from_address_id` when the owner may send as
   it. A reply mode without `parent_message_id` → 400 `invalid_field`; drafts are not parents (404).
8. **Undo / cancel → draft**: `outbound_id`, Message-ID, `in_reply_to` and message_refs are
   cleared on the restored draft (version + 1, un-trashed); the canceled outbound row stays as
   history. Undo of a draft → 409 `too_late`; of a shared copy, an inbound message or someone
   else's message → 404. `finish_cancel_schedule` on an already canceled outbound → 409
   `invalid_state`.
9. **Status**: every transition records a `local.*` event (`local.queued`, `local.sending`,
   `local.accepted`, `local.failed`, `local.canceled`, `local.schedule_local`,
   `local.rescheduled`; source keys `local:<n>`); `outbound.status` is emitted to every copy owner
   on each status change, `threads.changed` too when the Sent/Scheduled membership changes.
   `last_event` stores Resend's spelling (`delivered`, `opened`, …; local types verbatim) and never
   regresses on out-of-order events; `status_detail` comes from `bounce.message`,
   `failed.reason`, `suppressed.message|reason`, `reason`, `message`, `error` (≤ 1000 bytes) and
   is cleared by a status change without text. `mark_failed` is a no-op once Resend reported
   `sent` or later (a webhook outran a failing POST retry); `mark_accepted(scheduled=true)` for a
   send without `scheduled_at` is a plain acceptance. `accepted_at` is the first acceptance (also
   for Resend-scheduled sends, per the header).
10. **Retry** (`retry_failed_send`, `admin_retry_outbound`): a scheduled send keeps its time (and
    `scheduled_via`) when it is still ≥ 60 s ahead, otherwise it sends now.
11. **Loopback / spoofing**: an `X-AzMail-Ref` or Message-ID only identifies our own outbound when
    the From address equals that outbound's identity (both values are visible to recipients, so
    alone they could suppress or "un-spam" forged mail). A merge sets `in_inbox=1`, keeps the
    sender's copy read and marks a member's shared copy unread, and emits `mail.new` to non-senders.
    Late cleanup in `set_outbound_message_id` ORs `in_inbox` only from a normal (not spam/trash)
    loopback copy, re-points drafts whose parent was that copy, and merges the loopback copy's
    thread into the out copy's thread.
12. **Delivery**: `received_for` entries in `Name <addr>` form are accepted; a `Date` more than 24 h
    after `received_at` is replaced by `received_at`; an attachment is inline when it has a
    Content-ID and either an inline disposition or a `cid:` reference in the HTML; spam copies do
    not add contacts; the unroutable-to-admins policy applies only when an envelope recipient is
    in a local domain (`delivered_to` = the first such address). An email whose copies were all
    skipped as split-delivery duplicates is marked `delivered` and returns `Duplicate`.
13. **`mark_inbound_failed`** never downgrades a `delivered` email; for an unknown resend id it
    creates the row (source `webhook`); `error` is truncated to 500 bytes.
14. **WP0 stub tests** (see §I 11): "WP0 fixer stubs link" also asserts `mail::queue_send` throws
    `NotImplemented` (lines 697–698), which no longer holds; WP0 must drop those assertions too.


## I. Decisions made during the review-fix round (merged)
- **Signature**: the server never appends the settings signature; the frontend editor inserts `div[data-azm-signature]` exactly once above the quote.
- **Frozen send payload**: `payload_json` has a `"wire"` object (In-Reply-To/References) computed once before the first POST; all strings are valid UTF-8; retries under one Idempotency-Key send identical bytes. A user/admin retry creates a new outbound uuid and re-resolves headers.
- **Pending sends vs delete**: trash cancels only queued *scheduled* sends; delete-forever cancels any queued send; Resend-held scheduled sends or sends being POSTed → 409 `scheduled_send_pending` / `send_in_progress`.
- **Raw .eml GC**: an inbound raw blob is referenced only while its inbound row is pending/failed or a message still has that `inbound_id`. `attachment_count` counts normal (non-draft, non-trashed, non-spam) messages only. Spam purge ages by `MAX(date, created_at, updated_at)`.
- **Loopback trust**: X-AzMail-Ref only clears `spoofed_internal` when all local envelope recipients were on that send; the outbound Message-ID is captured from inbound mail only when the ref check and DKIM/DMARC pass. Reconcile is round-robin (kv `outbound.reconcile_cursor`); a poll event repeating an already recorded type is a Duplicate.
- **Login/auth**: disabled account → 403 `account_disabled` regardless of password (counted by the throttle); invalid/huge email → 401; login body limit 8 KiB; authenticated routes check the Bearer token before reading any body; throttle keys emails by sha256, caps maps at 100k entries, IPv6 per /64.
- **Shutdown**: `AZMAIL_SHUTDOWN_GRACE_SEC` (default 25) is the TOTAL graceful shutdown time (drain 5 s, jobs until 20 s, then in-flight HTTP transfers aborted via `net::CancelSignal`); hard `exit(1)` on overrun; `TimeoutStopSec=40` must stay ≥ grace + 10 s. Job lease 2 min with 30 s heartbeat; `Runner::on_abandoned` hook marks abandoned inbound fetches failed.
- **Resend upload deadline**: `RESEND_TIMEOUT_SEC + body / RESEND_UPLOAD_KBPS` (default 256 KiB/s), capped at 15 min.
- **Secrets**: `AZMAIL_SECRET` must be ≥32 bytes, contain no placeholder marker (CHANGE_ME…), and not be degenerate; `RESEND_WEBHOOK_SECRET` must be `whsec_` + base64 of ≥16 bytes. `doctor` reports violations.
- **Known gaps (not fixed)**: marking a thread containing a queued scheduled send as Spam leaves the send queued; disabling a user or revoking send-as does not cancel their already-queued sends; `repo::list_outbox(status=failed)` still lists superseded rows; signed-URL `exp` has no upper bound check; the Material Symbols font (~4 MB) is not subset.
