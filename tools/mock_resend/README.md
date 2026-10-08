# mock_resend — Resend API + minimal Cloudflare R2 for AZ Mail

A Python standard-library (≥ 3.10) emulation of the parts of the [Resend](https://resend.com) API
that AZ Mail uses, plus a minimal S3/R2 endpoint, so the E2E suite (`tests/e2e`) and local
development (`scripts/dev.sh`) run without any real network. It implements DESIGN.md §6 and
Addendum A.

```
python3 -m tools.mock_resend.server --port 8787 --s3-port 8788 \
    --webhook-url http://127.0.0.1:8080/api/webhooks/resend --local-domains azmail.test
python3 tools/mock_resend/server.py ...        # same, as a script
python3 -m tools.mock_resend.selftest -v       # self-test (also: python3 tests/e2e/test_mock.py)
```

Files: `server.py` (HTTP servers, Resend emulation, control endpoints), `store.py` (state),
`svix.py` (webhook signing + retrying sender), `eml.py` (raw MIME via the `email` package),
`sigv4.py` (pure AWS SigV4 signing and verification), `s3.py` (R2 endpoint), `selftest.py`.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--host` / `--port` | `127.0.0.1` / `8787` | Resend API listener (`0` = random port) |
| `--api-key` | `re_mock_000…` (or `$MOCK_RESEND_API_KEY`) | the only accepted Bearer key |
| `--webhook-url` | – | where signed webhooks are POSTed (changeable via `/_mock/config`) |
| `--webhook-secret` | fixed dev `whsec_…` | Svix signing secret (`whsec_<base64>`) |
| `--local-domains` | – | team domains: mail to them loops back as inbound mail |
| `--time-scale` | `1.0` | multiplier for internal delays (event spacing, webhook retries, delay@) |
| `--meta-delay` | `0` | seconds until `GET /emails/{id}.message_id` is non-null |
| `--rate-limit` | `10` | requests/second, fixed window (`0` = off) |
| `--reject-scheduled-attachments` | off | 422 for scheduled sends with attachments |
| `--received-for-mode` | `envelope` | how inbound `received_for` is filled (see *Envelope* below) |
| `--s3-port` | `-1` (off) | R2/S3 listener (`0` = random) |
| `--s3-access-key` / `--s3-secret-key` | dev values | the only accepted SigV4 credentials |
| `--s3-bucket` | `azmail` | comma list of existing buckets |
| `--s3-regions` | `auto` | accepted SigV4 regions (R2 uses `auto`) |
| `--s3-write-interval` | `1.0` | min seconds between writes of one key (R2: 1 write/s per key) |
| `--s3-data-dir` | – | persist objects on disk (dev); otherwise in memory |
| `--port-file` | – | write `{"port","s3_port","pid"}` JSON once listening |

## Resend API behaviour

* **Edge / auth.** A missing or empty `User-Agent` → **403 `error code: 1010`** (text/plain,
  like Cloudflare in front of Resend). No `Authorization` → 401 `missing_api_key`; a wrong key →
  403 `invalid_api_key` (real Resend behaviour; DESIGN §6 says 401 — AZ Mail maps both to
  `Kind::Auth`). More than `--rate-limit` requests in one wall-clock second → 429
  `rate_limit_exceeded` with `retry-after: 1` and `ratelimit-*` headers.
  Errors are `{"statusCode","name","message"}`.
* **`POST /emails`** validates `from` (address format; domain must be a local/verified domain →
  403 `validation_error`), `to` (required, string or array, ≤ 50), `cc`/`bcc`/`reply_to` (≤ 50
  each), `subject` (required, may be empty), `html` or `text` (one required), `headers` (string
  map), `tags` (`[{name,value}]`, `[A-Za-z0-9_-]{1,256}`), `attachments` (`filename` + strict
  base64 `content`; `path` is rejected; `content_id` < 128 chars; ≤ 40 MB base64 in total) and
  `scheduled_at` (ISO 8601 with zone only — natural language is rejected on purpose; within
  now…now+30 d on the mock clock; with `reject_scheduled_attachments` → 422 *"Emails with
  attachments cannot be scheduled."*). Returns `{"id": "<uuid>"}`. Custom `Message-ID` headers
  are ignored unless `honor_message_id` (F4.5 is unverified).
* **`Idempotency-Key`** (1–256 chars, else 400 `invalid_idempotency_key`, 24 h): the same key +
  same body replays the stored response byte-for-byte; a different body → 409
  `invalid_idempotent_request`; while the first request is still being processed → 409
  `concurrent_idempotent_requests`. Failed requests are not stored (the key stays usable).
* **`GET /emails/{id}`** → `{object, id, message_id, to, from, created_at, subject, html, text,
  bcc, cc, reply_to, last_event, scheduled_at, tags:[{name,value}]}` with **Postgres-style**
  timestamps (`2026-10-07 12:00:00.123456+00`). `message_id` is `null` until `meta_delay`
  seconds after the send (scheduled mail: after it fired). `GET /emails` lists sent mail.
* **`PATCH /emails/{id}`** `{scheduled_at}` and **`POST /emails/{id}/cancel`** work only while
  the email is still scheduled; otherwise 422 `validation_error`. Cancel sets `last_event`
  `canceled` (Resend sends no webhook for it).
* **`GET /domains`**, **`GET /domains/{id}`** (records incl. the receiving MX
  `inbound-smtp.us-east-1.amazonaws.com` prio 10), **`GET /api-keys`** — for admin domain status
  and `azmail doctor`.

### Delivery simulation

When a send is accepted (or a scheduled send fires) the mock emits, `delivery_delay` + n ×
`event_spacing` seconds later (× `--time-scale`), per **email** (DESIGN B3; `data.to` lists the
recipients):

| Recipient local part starts with | Events |
|---|---|
| `fail` | `email.failed` (only) |
| `bounce` | `email.sent`, `email.bounced` (`bounce:{type,subType,message}`) |
| `suppress` | `email.sent`, `email.suppressed` |
| `delay` | `email.sent`, `email.delivery_delayed`, then 2 s later `email.delivered` |
| `complain` | `email.sent`, `email.delivered`, `email.complained` |
| anything else / local domain | `email.sent`, `email.delivered` |

Scheduled sends emit `email.scheduled` first. Event `data` carries `email_id`, `created_at`,
`from`, `to`, `subject`, `tags` **as a map** and `message_id` once it is visible.

**Loopback** (recipients on `--local-domains`): a raw .eml is built (`eml.py`: Message-ID = the
send's message id, `In-Reply-To` / `References` / `X-AzMail-Ref` and other request headers,
attachments with inline parts as `multipart/related`, RFC 2047 subject, RFC 2231 filenames,
`Authentication-Results` pass), the receiving MX prepends its `Received` trace header (see
*Envelope*), and it is stored as a received email with `received_for` per `received_for_mode`
(default: the local envelope recipients **including bcc**), `headers` lowercased, `authentication`
all `pass`, and the send's full `bcc` list exposed on every copy (worst case — AZ Mail must drop
it, DESIGN C2). An `email.received` webhook follows.

### Envelope (`received_for`)

Resend documents `received_for` as the recipient addresses taken from the **`for` clause of the
`Received` headers**. Whether that is the complete envelope is *unverified* (DESIGN F4.1;
`tools/resend_probe.py` check 1 records `received_for` next to the raw `Received` headers): a
receiving MTA names the recipient in its `for` clause for a single-recipient SMTP transaction, but
Postfix, Exim and Sendmail leave the clause out when one transaction carries several `RCPT TO`, and
RFC 5321 §4.4 allows at most one address there. AZ Mail routes by `received_for` and falls back to
the `To` ∪ `Cc` headers only when it is empty (DESIGN C1), so the difference matters:

| Resend gives | AZ Mail delivers to |
|---|---|
| every local envelope recipient | everyone, Bcc'd team members included |
| `[]` or no field | the local `To`/`Cc` addresses — a Bcc-only team member gets nothing |
| only some recipients | only those — the others get nothing (nothing is logged as unroutable) |

The mock prepends one `Received: from … by inbound-smtp.us-east-1.amazonaws.com with SMTP id …
[for <rcpt>]; <date>` header per delivery (one delivery = one SMTP transaction; with
`split_delivery` one per envelope recipient, so the copies' raw files differ only in it) with a
`for` clause only when the delivery has exactly one recipient. `received_for_mode` then decides
what the API reports:

| `received_for_mode` | `received_for` |
|---|---|
| `envelope` (default) | all local envelope recipients incl. bcc — the idealised assumption the rest of the suite runs on |
| `received` | the `for` clauses of the stored raw message: `[rcpt]` for a single-recipient (or split) delivery, `[]` otherwise |
| `first` | the `for` clause names the first recipient even when there are several → a partial list |
| `empty` | always `[]` |
| `omit` | the field is absent from `GET /emails/receiving/{id}` and the `email.received` webhook |

An explicit `received_for` in `POST /_mock/inbound` (even `[]`) is used as the envelope and
reported verbatim whatever the mode. E2E 36 (`tests/e2e/scenarios/s36_envelope_fallback.py`)
covers `omit`, `received` and `received` + `split_delivery` against the real backend.

### Receiving

* `GET /emails/receiving?limit=&after=&before=` — newest first; `limit` 1–100 is **required**
  here (stricter than Resend, AZ Mail always pages); `after` = older items, `before` = newer
  items; `has_more`; unknown cursor → 422.
* `GET /emails/receiving/{id}?html_format=cid|data_uri` (default `data_uri`: inline `cid:`
  references become data URIs) → `{…, html_format, headers, received_for, authentication,
  message_id, raw:{download_url, expires_at}|null, attachments:[…]}`.
* `GET /emails/receiving/{id}/attachments[?limit&after&before]` (default limit 20, like Resend)
  and `…/attachments/{att_id}` → fresh `download_url`s.
* Download URLs are `http://<host>/_dl/<token>?exp=<unix>` (TTL `download_ttl`, 1 h): they
  **302** to `/_blob/<token>?exp=…`, enforce expiry (also against a tampered `exp`) and **reject
  any `Authorization` header** with 400, recording a violation (`GET /_mock/violations`) — this
  catches credential leaks to presigned URLs.

### Webhooks

Svix-signed (`svix-id`, `svix-timestamp` = wall clock, `svix-signature: v1,<base64>`), delivered
in order by one worker; each delivery is attempted at 0 s, +1 s, +3 s (× `--time-scale`) until a
2xx answer, re-signed per attempt with the same svix-id. Every attempt is logged
(`GET /_mock/webhooks`).

## Control endpoints (no auth)

| Endpoint | Body / query | Effect |
|---|---|---|
| `GET /_mock/health` | – | `{ok:true}` |
| `GET /_mock/state` | – | `{seq, now_ms, offset, config, emails, received, pending_webhooks, idle, faults, …}` |
| `GET/POST /_mock/config` | `{webhooks_enabled, webhook_url, shuffle_events, duplicate_webhooks, split_delivery, received_for_mode, strip_custom_headers, strip_thread_headers, meta_delay, raw_missing, reject_scheduled_attachments, honor_message_id, verify_from_domain, local_domains, rate_limit, download_ttl, event_spacing, delivery_delay}` (partial) | merge config |
| `GET/POST /_mock/faults` | `[{match:"POST /emails", status, name, message, retry_after, body, timeout, delay, close, count, target}]` or `{faults, append}` | replace (or append) the fault list |
| `POST /_mock/advance?seconds=N` | – | move the mock clock; fires due scheduled sends → `{now_ms, offset, fired:[ids]}` |
| `GET /_mock/sent?since=&method=&path=` | – | `{seq, requests:[{seq, at, method, path, query, user_agent, idempotency_key, auth, status, name, replayed, fault, email_id, body}], emails:[…]}` (attachment bytes replaced by length + sha256) |
| `GET /_mock/emails/{id}` | – | full sent email incl. `message_id`, events, attachment hashes |
| `GET /_mock/received[?since]`, `/_mock/received/{id}[?raw=1]`, `/_mock/received/{id}/raw` | – | stored inbound mail / raw .eml |
| `POST /_mock/inbound` | `{from, to, cc, bcc, reply_to, received_for, subject, subject_charset, html, text, attachments:[{filename, content_type, content(b64)\|text, content_id, inline}], headers, message_id, in_reply_to, references, spf, dkim, dmarc, date, webhook}` | inject external mail → `{ids, message_id, received_for:[per id], raw_sha256 (of the first copy)}` (one id per envelope recipient with `split_delivery`; envelope = `received_for` if given, even `[]`, else the local to/cc/bcc) |
| `POST /_mock/webhook` | `{email_id[, type, svix_id]}` or `{payload[, svix_id]}` | (re-)send a signed event |
| `GET /_mock/webhooks?since=&wait=1` | – | delivery attempts log |
| `GET /_mock/violations` | – | Authorization headers seen on download URLs |
| `GET /_mock/s3?since=` | – | `{buckets, objects:[{bucket,key,size,sha256,content_type,etag}], requests:[{method,path,auth,auth_ok,status,code}]}` |
| `POST /_mock/reset` | `{keep_data?, s3?}` | config/faults/clock/violations back to start; without `keep_data` also all Resend data; `s3:true` wipes the bucket |

Faults match `fnmatch` patterns over `"METHOD /path"` (no query), are consumed in list order,
`count` defaults to 1 (`null` = unlimited). `status` answers immediately with a Resend error
(or `body` verbatim, e.g. an HTML 502); `timeout: s` **processes the request** and then holds
the response (tests idempotent replays after client timeouts); `delay: s` holds the request
before processing while its idempotency key is in flight; `close` drops the connection.
`target: "s3"` applies the fault to the R2 endpoint (XML error with `name` as the Code).

## R2 / S3 endpoint

Path-style `/<bucket>/<key>`: `PUT`, `GET` (incl. `Range`), `HEAD`, `DELETE` (always 204),
`HEAD /<bucket>`, `GET /<bucket>?list-type=2&prefix=`, `GET /` (buckets). Every request must be
SigV4-signed — Authorization header or presigned query — and is verified exactly like S3
(`sigv4.verify`): credential scope `<date>/auto/s3/aws4_request`, `x-amz-date` within 15 min,
`x-amz-content-sha256` required (real hex for PUT, checked against the body →
`XAmzContentSHA256Mismatch`; `UNSIGNED-PAYLOAD` allowed; `STREAMING-*` → 501), `host`,
`x-amz-date`, `x-amz-content-sha256` and every other `x-amz-*` header must be signed, presigned
`X-Amz-Expires` 1…604800, expiry checked after the signature (`AccessDenied` *Request has
expired*). R2 specifics: PUT without `Content-Length` (or chunked) → 411
`MissingContentLength`; a second write of the same key within 1 s → 429 `TooManyRequests`;
`response-content-type` / `response-content-disposition` (and the other `response-*`) overrides
on GET/HEAD; unknown bucket → `NoSuchBucket`, missing key → `NoSuchKey`; S3 XML errors
(`<Error><Code/><Message/>…</Error>`, plus `StringToSign` / `CanonicalRequest` on signature
mismatches to ease debugging); no security headers (no `nosniff`), `Server: cloudflare`.

## Self-test

`python3 -m tools.mock_resend.selftest` reproduces the Svix docs vector
(`v1,rAvfW3dJ/X/qxhsaXPOyyCGmRKsaKWcsNccKXlIktD0=`) and the Svix library vector, every AWS SigV4
S3 example from DESIGN A.2 (signing key, GET object, PUT object, lifecycle, list, presigned GET)
for signing *and* server-side verification, and runs the flows above against in-process servers.

## Known deviations from real Resend

* Wrong API key: 403 (real) instead of DESIGN §6's 401; both are `Kind::Auth` for AZ Mail.
* `scheduled_at` natural language is rejected (valid on Resend; AZ Mail must never send it).
* `GET /emails/receiving` requires `limit` (optional on Resend).
* Attachments by `path` are not supported.
* Delivery events are synthesized per email from recipient prefixes; open/click events only via
  `POST /_mock/webhook`.
* `received_for` defaults to the complete local envelope (incl. bcc). Real Resend derives it from
  `Received` `for` clauses and may report fewer addresses or none for multi-recipient mail
  (unverified, see *Envelope*); use `received_for_mode` to emulate that.
