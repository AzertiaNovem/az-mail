# AZ Mail — REST / WebSocket API (wire contract)

> Mirrors `frontend/src/api/types.ts` exactly. Backend serde (`backend/src/mail/serde.*`, `backend/src/api/dto.*`) must produce these shapes.

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

---

## Addendum B — conventions fixed during WP0 (binding for backend serde and frontend)

### General rules
1. DELETE routes return **204** with an empty body (labels, drafts, admin users/aliases/domains). `POST /api/auth/logout` and `POST /api/auth/password` also return 204.
2. Unpaged collection GETs return **bare JSON arrays**: `GET /api/labels` → `Label[]`; `/api/identities` → `Identity[]`; `/api/admin/users` → `AdminUser[]`; `/api/admin/aliases` → `AdminAlias[]`; `/api/admin/domains` → `DomainRow[]`; `/api/admin/inbound?state` → `InboundRow[]`; `/api/admin/outbox?status` → `OutboxRow[]`; `/api/admin/jobs?state` → `JobRow[]`.
3. `GET /api/admin/events?type&cursor` → `{items: WebhookEventRow[], next_cursor: string|null}`.
4. Values stored in TEXT JSON columns (`recipients_json`, `jobs.payload`, `detail_json`) are sent as **parsed JSON values**, never JSON-encoded strings (`InboundRow.recipients: string[]`, `JobRow.payload: object`, `DeliveryEvent.detail: object`, `{}` when empty).
5. Error details: 429 `too_many_attempts` → `details.retry_after` (seconds); 409 `version_conflict` → `details.current` (full `Draft`, for both `PUT /api/drafts/:id` and `POST /api/drafts/:id/send`); 400 `invalid_field` → `details.field`; 422 `unknown_local_recipient` → `details.emails: string[]`; 422 `too_many_recipients` → `details.field`.
6. `POST /api/auth/password` with a wrong `current_password` → **403** `invalid_credentials` (never 401; 401 always means "session ended").
7. `Draft.mode` is always present (`'new'` by default), never null.
8. Signed file URLs carry `u=<user_id>`: `/api/files/:id?d=i|a&u=<uid>&exp=<ms>&sig=<b64url>`, `/api/files/raw/:messageId?u=&exp=&sig=`. Clients treat them as opaque.
9. `GET /api/admin/stats` includes `storage: {backend:'local'|'r2', delivery:'proxy'|'redirect', blob_count:number, blob_bytes:number}` (DESIGN Addendum A).

### Route → response
- `PUT /api/settings` → the full `Settings` after the partial update.
- `GET /api/labels/:id` → `Label`; `POST /api/labels` → 201 `Label`; `PATCH /api/labels/:id` → `Label`.
- `GET /api/messages/:id/events` → `{events: DeliveryEvent[]}`.
- `POST /api/admin/users` → 201 `AdminUser`; `PATCH /api/admin/users/:id` → `AdminUser`.
- `POST /api/admin/aliases` → 201 `AdminAlias`; `PATCH /api/admin/aliases/:id` → `AdminAlias`.
- `GET /api/admin/domains/:id` → `DomainRow`; `POST /api/admin/domains` → 201 `DomainRow`; `GET /api/admin/domains/:id/status` → `DomainStatus`.
- `POST /api/admin/outbox/:id/retry` → the **new** `OutboxRow` (new id and uuid; the old row stays `failed`); clients invalidate the `['admin','outbox']` prefix.
- `POST /api/admin/jobs/:id/retry` → `JobRow`.
- `POST /api/admin/sync` → 202 `{job_id: number}`.

### Row shapes
```ts
interface DeliveryEvent { type:string; occurred_at:number; detail:object }
interface DomainRow { id:number; name:string; receiving_enabled:boolean; created_at:number }
interface DomainStatus { id:number; name:string;
  resend: { id:string; status:'not_started'|'pending'|'verified'|'failed'|'temporary_failure'|string;
            region:string|null; created_at:number|null; records:DomainDnsRecord[] } | null }
interface DomainDnsRecord { record:string; name:string; type:string; ttl:string; status:string; value:string; priority?:number } // priority omitted when absent
interface WebhookEventRow { id:number; svix_id:string; type:string; resend_email_id:string|null; received_at:number;
  processed_at:number|null; result:string|null }      // no payload: admins never see mail bodies
interface InboundRow { id:number; resend_id:string; state:'pending'|'delivered'|'unroutable'|'failed'; source:'webhook'|'poll'|'admin';
  message_id_header:string|null; from_email:string|null; subject:string|null; received_at:number|null;
  recipients:string[]; error:string|null; created_at:number; updated_at:number }
interface OutboxRow { id:number; uuid:string; sender_user_id:number; sender_email:string; from_email:string; status:OutboundStatus;
  status_detail:string|null; error_name:string|null; scheduled_at:number|null; scheduled_via:'resend'|'local'|null;
  resend_id:string|null; total_bytes:number; last_event:string|null; last_event_at:number|null; created_at:number; updated_at:number }
interface JobRow { id:number; kind:string; lane:string; priority:number; payload:object; state:'pending'|'running'|'done'|'dead'|'canceled';
  run_at:number; attempts:number; max_attempts:number; locked_until:number|null; dedupe_key:string|null; last_error:string|null;
  created_at:number; updated_at:number }
```

### WebSocket frames
Server frames are **flat**: the Hub serializes the fields of `data` beside `type` (`{type, ...data}`), never `{type, data:{…}}`. Events with no contents are `{type}`. `mail.new.from` is an Address `{name,email}`. `outbound.status.status_detail` is `string|null`.

## Addendum B.1 — additive fields and behaviour fixed during the review (be_infra)

### `GET /api/admin/stats` (additive)
```ts
interface AdminStats {
  // … every field of Addendum B / the table above, unchanged …
  queue: { pending:number; dead:number;
           periodic:number };            // of `pending`: periodic jobs (dedupe "periodic:<kind>", always queued)
  poll_gap: { detected_at:number;          // ms epoch: when the poller last detected the gap
              detail:string } | null;      // Chinese, no mail content; null when no warning
}
```
- `queue.periodic` counts the pending/running jobs whose dedupe key is `periodic:<kind>` (poll, reconcile, gc …). They are always present, so the real backlog is `queue.pending - queue.periodic`.
- `poll_gap` is the `poll.receiving` gap warning (DESIGN B7, kv `poll.gap_warning`): mail older than the last sync position that had never been seen (webhook delivery failed), or the last sync position was not found again (outage beyond Resend's 30-day retention or more than 20 pages — mail of that period may be lost). It is shown for 7 days after `detected_at` and then dropped (the next poll deletes it); a new detection replaces it. Clients show it as a warning banner.

### `POST /api/auth/login` (behaviour)
- Body limit 8 KiB (413 `payload_too_large` above it). An `email` that is not a valid address or is longer than 254 bytes is answered like an unknown account: 401 `invalid_credentials` (it still counts against the client IP's throttle).
- A disabled account answers 403 `account_disabled` **whatever the password** (it no longer confirms a correct guess), and every such attempt counts against the throttle like a wrong password.
- The per-IP throttle counts IPv6 clients per /64.

### Authenticated routes with a body (behaviour)
- Every `U`/`A` route with a request body checks the Bearer token **before** reading the body: 401 `unauthorized` (403 `forbidden` for non-admins on `A` routes) with `Connection: close`, whatever the body size. The 413 `payload_too_large` check on `Content-Length` still comes first.
