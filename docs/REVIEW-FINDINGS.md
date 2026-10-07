# Review findings (pending fixes)

Verified by independent skeptics. 54 kept, 3 refuted. Severity: {'high': 7, 'medium': 16, 'low': 31}

## s07 flake
- Root cause: Timing, not anything left over from s01-s06. s07 failed in isolation 3 times out of 6 runs, and all 8 kept runs fit one pattern. (1) Scenario: the injected reply had the subject "Re: <subject>" and came from the person alice had mailed. The C7 subject fallback (reply prefix, same normalized subject, within 7 days, a participant in common) put it into alice's sent thread right away, so the meta_delay/B2 race was never exercised. The scenario then looked for an inbox thread whose own subject was "Re: 报价单 ...". A thread's subject is the subject of its earliest message by messages.date. (2) Backend: for inbound mail, messages.date comes from the Date header, which only has whole seconds, while alice's own copy is dated to the millisecond. If the send and the reply fell in the same second (send at .495, reply Date .000), the reply sorted first, the thread became "Re: ..." and the test passed. If the reply crossed into the next second (send at .965, Date at the next .000), the thread kept the original subject, no "Re:" thread ever appeared, and the scenario timed out after 30 s. The backend log looked like only GET /api/threads polling because the reply had already been fetched and filed into the existing thread. The same truncation has a visible effect in production: an inbound reply that arrives within the same second as the mail it answers sorts above it and takes over the thread subject. It happened in s06 too, where alice's thread was titled "Re: 项目讨论 ...".
- Fix: Scenario (tests/e2e/scenarios/s07_meta_race.py): the partner now gives the reply a new subject without a reply prefix, so only its In-Reply-To/References link it to alice's mail. The scenario finds the reply by scanning messages rather than by thread subject. It checks that the reply has its own thread while the Message-ID is still unknown (alice's message is read after the reply, so the check holds), then that both end up in one thread by reference after capture, in the order out then in, under the original subject. Backend (backend/src/mail/inbound.cpp): a new helper message_date() collects the existing date rules (Date header, else receipt time, bogus future Date clamped). It adds one rule: if the Date header is a whole second and Resend's millisecond receipt time falls inside that second, the receipt time is stored instead. That is the same instant measured more precisely, so a reply received in the same second no longer sorts before its parent. Unit test added in backend/tests/unit/test_delivery.cpp, "dates: a whole-second Date header is refined by a receipt inside that second". It covers the same second, a later second, a sender clock slightly ahead, and a header that is not a whole second, plus the thread subject and message order. With the new rule disabled it fails (thread subject "Re: 报价单", reply listed first); with the fix it passes. Results: backend unit tests 472/472; s07 alone passed 5 runs out of 5; the full suite passed 3 times in a row (r2 35/35 and local smoke 4/4 each time); tests/e2e/test_harness.py OK. In the kept databases of all three full runs the reply fell in the same second as the send, the case that used to flip, and it was stored with its millisecond receipt time after alice's message; alice's s06 thread kept the subject "项目讨论 ...".
- Branch `worktree-wf_54b12e3c-e78-1` @ 5d872796d26321785dbee4ba587648b2b3a96506 (not merged yet); E2E: 3 of 3 consecutive full-suite runs passed (r2: 35 passed, 0 failed; local blob store smoke: 4 passed, 0 failed, each run); before the fix s07 alone failed 3 of 6 runs

## Orchestrator browser observations
- Latest message in a thread stays collapsed after sending a reply (Gmail expands it).
- Collapsed thread messages show no snippet.
- Explicit logout shows "登录已过期".
- Admin aliases table wraps the address per character at narrow widths.
- Stats counts periodic jobs as pending.

## [high] frontend/F1: Forwarding drops the original message's attachments: the compose window never adopts the attachments the server copied, so the next PUT or the send deletes them
- File: `frontend/src/components/compose/ComposeWindow.tsx`:349 — verdict: confirmed
- Scenario: Reproduced with a scratch vitest test (forward.review.test.tsx). Open 转发 on a message with 合同.pdf and type a recipient: POST include_parent_attachments=true, and the server returns attachments [71]. Edit the subject: PUT /api/drafts/900 is sent with attachment_ids: [] and no attachment chip is shown. The server deletes attachment 71, so the forwarded mail goes out without 合同.pdf. Sending straight after the first autosave fails the same way: sendDraft's `draft: collect()` carries attachment_ids [].
- Fix: Client side (preferred, no contract change): add an `onCreated?: (draft: Draft) => void` option to useAutosave (useAutosave.ts UseAutosaveOptions, called from onSaved when created=true). In ComposeForm (ComposeWindow.tsx) implement it to merge every `draft.attachments` entry with `!a.inline` whose id is not already in fieldsRef.current.attachments into the fields. Assign fieldsRef.current synchronously and then call setFieldsState, without markDirty. The synchronous fieldsRef update matters: DraftAutosaver.save() calls onSaved before prepareSend() resolves, so the send's collect() then lists the copied ids too. The files then appear in AttachmentBar, can be removed on purpose, and count toward the size check in addFiles. Leave copied inline quote images alone, since the server keeps them through the quote reference. Add a test that forwards, saves twice and sends, then asserts that attachment_ids contains the copied id.

## [high] mail-logic/R2: Delete-forever or purge of a queued/scheduled send leaves the outbound live; it is sent later without its attachments and can no longer be canceled
- File: `backend/src/mail/mailbox.cpp`:635 — verdict: confirmed
- Scenario: Alice schedules a mail with 合同.pdf for tomorrow (local scheduling), changes her mind, and trashes then deletes forever the thread. Tomorrow the outbound.send job still sends the mail, without the PDF. Scratch test R2: status=queued, attachments before=1 after=0, job_state=pending.
- Fix: (a) mailbox.cpp apply_to_thread: for Trash/DeleteForever of a sender copy (is_shared_copy=0) whose outbound is LocalPending/queued (undo window or local schedule), cancel in the same tx (UPDATE outbound SET status='canceled' WHERE status='queued' plus jobs::cancel(job_id), like undo_send/begin_cancel_schedule) before trashing or deleting. For a Resend-scheduled row (status accepted/scheduled, scheduled_via=resend, scheduled_at > now), refuse with 409 (cancel the schedule first), because the network cancel cannot run inside the tx. In DeleteForever and in purge_trash's victim query, add `AND NOT EXISTS (SELECT 1 FROM outbound o WHERE o.id = messages.outbound_id AND messages.is_shared_copy = 0 AND (o.status IN ('queued','sending') OR (o.status IN ('accepted','scheduled') AND o.scheduled_at > ?)))`. (b) outbound_jobs.cpp SendAttempt::run / outbound.cpp load_send_plan: if plan.attachments.size() != payload.attachment_ids.size(), call fail('attachment_missing', kZhAttachmentMissing) and throw Permanent. If no is_shared_copy=0 non-draft copy exists for the outbound, mark it canceled instead of sending.

## [high] runtime-wire/RT-1: HEAD of an existing R2 object larger than 64 KiB fails with a non-retryable BlobError, which breaks blob dedupe in R2 mode
- File: `backend/src/net/http_client.cpp`:336 — verdict: confirmed
- Scenario: Reproduced twice. (1) Scratch program /private/tmp/claude-501/review-rt/r2_head.cpp using the real R2BlobStore against the mock S3: put_bytes of 1 KB twice works; put_bytes of 70 KB twice → second call throws 'blob larger than allowed (retryable=0)'. (2) Scratch copy of E2E s14 with the PDF enlarged to 100 KB, R2 backend: alice→bob never arrives. Log shows `job dead kind=inbound.fetch attempts=1 error="storage: blob larger than allowed"` and inbound_emails.state='failed'. Production impact with R2, the recommended backend: (a) internal mail with any attachment over 64 KiB is never delivered to local recipients, because the loopback attachment has the sender's sha; (b) uploading a file that already exists (same file attached twice, or by another user) returns 502 storage_error; (c) any inbound attachment already stored (repeated logos, forwarded PDFs) marks the whole inbound mail failed; (d) `azmail blobs-migrate --to r2` uploads each blob over 64 KiB, then fails on the post-upload exists() check and never flips the row.
- Fix: backend/src/net/http_client.cpp, exchange(): run the declared-length check only when a body will actually be read, i.e. `if (dest != BodyDest::Truncated && !parser.is_done())`. parser.is_done() is already true right after the header for HEAD (skip), 1xx/204/304 and Content-Length: 0, and the body loop is already skipped in those cases. Optionally, also pass max_body=kMaxObjectBytes for HEAD in R2BlobStore::head().

Tests:
- http_client unit test: HEAD answered with `Content-Length: 1000000` and max_body=64 KiB must return 200 with body_size 0.
- Integration test against the mock S3: put_bytes of 100 KB twice, plus exists().
- E2E: s14 (or a variant) with a loopback attachment over 64 KiB.
- blobs-migrate test with a blob over 64 KiB.

## [high] runtime-wire/RT-2: Inbound attachments beyond the first 20 are silently dropped: the attachment list endpoint is never paginated
- File: `backend/src/resend/client.cpp`:429 — verdict: confirmed
- Scenario: Reproduced with a scratch E2E scenario (/private/tmp/claude-501/review-rt/repo/tests/e2e/scenarios/s90_many_attachments.py, R2 backend, my build): external mail to alice with 25 attachments → 'alice's copy has 20 of 25 attachments'. A direct mock call shows `returned 20 of 25; has_more = True` for exactly the request the client sends. Real Resend documents the same default (limit 20, has_more).
- Fix: resend/client.cpp, Client::list_received_attachments():
- Request `?limit=100` and loop with `&after=<id of the last item>` while has_more is true, bounded (e.g. 20 pages); stop if a page is empty or repeats a cursor.
- Return the concatenated list.

jobs/inbound_jobs.cpp FetchAttempt::run(): if `listed.size() < rcv.attachments.size()`, log a warning and throw Retry instead of delivering a partial set.

Tests:
- Unit test with a fake HttpClient returning two pages (has_more=true, then false) that asserts the second request carries after=<last id>.
- E2E scenario injecting 25 attachments via /_mock/inbound that asserts all 25 arrive.

## [high] security/SEC-1: Quadratic html_to_text runs inside write transactions; one crafted email can stall all DB writes
- File: `backend/src/mail/html_text.cpp`:186 — verdict: confirmed
- Scenario: An external sender emails any team address with an HTML-only body of about 1-8 MB of '<head><body>' repeated. The inbound.fetch job calls deliver_inbound inside a write transaction and spends minutes to hours in make_snippet and fts_reindex while holding the SQLite write lock. Every other write (logins creating sessions, draft saves, sends, webhooks, job leases) waits out busy_timeout and returns 503. The same thing happens with an authenticated PUT /api/drafts/:id carrying up to 8 MiB of that HTML, or with a 1 MiB signature_html.
- Fix: 1) html_text.cpp skip_element_content: replace the find_close_tag + '<body' loop with one forward scan from content_begin that returns at whichever comes first, '</head' or '<body'. Alternatively, cache 'no </head> after pos X' in html_to_text so the miss is paid once. Apply the same cache idea to any find_close_tag miss: when it returns npos, later calls with the same name and a larger pos are npos too. 2) html_to_text </a> handling (html_text.cpp:488-493): pass link_worth_showing a string_view into the builder, not a std::string copy, and compare only a bounded prefix (for example 2 KB). If text is longer than href plus a margin, it cannot equal the href. 3) Cap the input: truncate inbound/draft HTML to a few MB before html_to_text in make_snippet and fts_reindex. The FTS body is truncated to 256 KB afterwards anyway. 4) Move the CPU work out of the write transaction. In jobs/inbound_jobs.cpp, compute the snippet and FTS body text before svc_.db.write and pass them in InboundEmail/DeliveryOptions; have fts_reindex accept a precomputed body. Do the same in mail/drafts.cpp apply_draft_input and finish_draft_write, and in queue_send freeze: compute html_to_text and make_snippet in the API handler before Pool::write. 5) Add a regression test: html_to_text and make_snippet of 1 MB '<head><body>' and of '<a>'x N + text + '</a>'x N finish in under 1 s.

## [high] spec-deploy/F1: nginx-api.conf: per-location proxy_set_header drops X-Forwarded-For/Host/X-Real-IP/X-Forwarded-Proto, so every client is 127.0.0.1
- File: `deploy/nginx-api.conf`:64 — verdict: confirmed
- Scenario: Production behind the shipped nginx config. In one 15-minute window, 20 failed logins happen across the whole team: normal typos, or one attacker anywhere on the internet posting wrong passwords. After that, every POST /api/auth/login and POST /api/auth/password from every user, even with the correct password, gets 429 too_many_attempts for up to 15 minutes. An attacker can repeat this indefinitely and keep the whole team locked out. Admin audit logs show 127.0.0.1 for everything.
- Fix: Add deploy/nginx-azmail-proxy.conf with `proxy_set_header Host $host; proxy_set_header X-Real-IP $remote_addr; proxy_set_header X-Forwarded-For $remote_addr; proxy_set_header X-Forwarded-Proto $scheme;`. Overwriting with $remote_addr is safe because nginx is the edge proxy, and it removes any client-supplied value. Include the snippet in all four locations (/api/ws, /api/webhooks/resend, /api/files/, /api/), or repeat the lines inline. Remove the server-level block, or keep it with a comment warning about the inheritance rule. Document the snippet install path (/etc/nginx/snippets/) in DEPLOY.md §9 and correct the claim at line 237. Optionally add a doctor or E2E check that a proxied request is recorded with a non-loopback remote_ip.

## [high] spec-deploy/F2: Placeholder AZMAIL_SECRET in azmail.env.example passes validation and doctor reports OK, so signed file URLs can be forged
- File: `deploy/azmail.env.example`:66 — verdict: confirmed
- Scenario: An operator fills in Resend and R2 but forgets the AZMAIL_SECRET line. The service starts and doctor is green. The secret is public in the repo, so anyone can compute sig for `/api/files/raw/<messageId>?u=<uid>&exp=<future>&sig=…` and `/api/files/<attId>?d=a&u=…`, enumerating the small sequential user and message ids. That gives unauthenticated download of every user's raw .eml (full mail bodies) and every attachment. The same applies to the webhook secret: Svix signatures can be forged.
- Fix: In deploy/azmail.env.example, ship `AZMAIL_SECRET=` and `RESEND_WEBHOOK_SECRET=` empty, with the openssl and Resend-console instructions in the comments. Startup then fails with "AZMAIL_SECRET is required" until the secret is set. In validate_config (backend/src/app/config.cpp, the security block at ~551), also reject secrets that contain "change-me", equal the shipped placeholder, or have very low entropy (for example fewer than 8 distinct bytes). In the Resend block (~601), reject RESEND_WEBHOOK_SECRET values whose body is a single repeated character or whose decoded key is under 16 bytes. doctor reuses validate_config, so it fails too. As defense in depth, make SignedUrls::verify_file/verify_raw reject exp > now_ms + ttl_ms_ + 1h.

## [medium] frontend/F2: A reply that has just been sent stays collapsed in the thread, because its id was already known as a draft (Gmail-parity bug 1, root cause)
- File: `frontend/src/components/mail/ThreadView.tsx`:131 — verdict: confirmed
- Scenario: Reproduced with a scratch vitest test (expand.review.test.tsx). Thread 1 has [msg 10 from 张三, msg 11 draft by Alice]. Turning msg 11 into a sent message (same id, is_read true) and refetching leaves only '张三：季度周报' as an expanded <article>. Alice's new reply is a collapsed row.
- Fix: In ThreadView's expansion effect, track a `knownSent` Set of ids already seen as non-draft, seeded with the non-draft ids on the first detail, instead of all ids. On each detail, fresh = detail.messages.filter(m => !m.is_draft && !knownSent.has(m.id)). Add those ids to knownSent, then expand fresh messages that are unread or equal the newest non-draft (`nonDraft[nonDraft.length-1]`). Optionally collapse the previously newest read message to match Gmail, and scroll the new one into view.

## [medium] frontend/F3: After an expired session and re-login, restored compose windows come back blank and create a second draft
- File: `frontend/src/components/compose/ComposeWindow.tsx`:149 — verdict: confirmed
- Scenario: Reproduced with a scratch vitest test (remount.review.test.tsx). Open a new message, type the subject 周报, and wait for the store to show draftId 900. Then unmount and remount ComposeDock, as happens on the /login round-trip. The store still shows {draftId:900, init:{kind:'new'}}, but the subject field is '', and the next edit calls createDraft a second time (2 POSTs, 0 GETs of draft 900).
- Fix: In useComposeSeed, derive `const init = win.draftId !== null && win.init.kind !== 'draft' ? { kind: 'draft', draftId: win.draftId } as const : win.init`. A mounted form is unaffected because `frozen` already holds its seed. Alternatively, in useAutosave onSaved(created), patch `{ draftId: draft.id, init: { kind: 'draft', draftId: draft.id } }`. With either change, the remount after re-login loads GET /api/drafts/:id (the cache was cleared, so it fetches the current version) instead of seeding a blank or new reply.

## [medium] frontend/F4: Compose signature controls can't work: the server re-appends the signature at send when the body text lacks it
- File: `frontend/src/components/compose/ComposeWindow.tsx`:938 — verdict: confirmed
- Scenario: Set the signature to `<p>— 张三</p>`, then compose and pick 不使用签名 so the block is removed from the editor. On send, the frozen HTML still contains `<div class="azm-signature">— 张三</div>`. With the signature `<img src="https://cdn…/logo.png">`, the new body already contains the logo and the server appends it again.
- Fix: Make the client authoritative. Add an additive DraftInput/DraftSendRequest.draft field `signature_handled?: boolean` (api/types.ts, docs/API.md Addendum, serde parse_draft_input) that the web client sends on create, update and send. Persist it on the draft row (additive nullable column, or alongside draft_mode). In queue_send_impl (drafts.cpp:676-678), pass an empty signature to freeze_html when the draft has it set. Other API clients and the E2E suite keep the server-side append. With this flag, 不使用签名, edited signatures and image-only signatures are all sent exactly as the editor shows them.

## [medium] mail-logic/R1: An 8-bit byte in an inbound Message-ID/In-Reply-To/References breaks the reply's frozen payload: the send fails permanently and Undo returns an empty draft
- File: `backend/src/mail/send_internal.cpp`:100 — verdict: confirmed
- Scenario: An external sender (or attacker) mails alice with a header like `Message-ID: <caf\xE9@ext.example>`. Alice replies with undo set to 30 s and clicks Undo, and the restored draft's html is ''. If she doesn't undo, the send fails with an opaque validation error and retrying cannot fix it. Reproduced in scratch test R1: plan.from='' to.size=0 html.size=0, and the restored draft html=''.
- Fix: (1) eml.cpp parse_msgid_list `add` lambda: after normalize_message_id, apply utf8_sanitize, the same function threads.cpp:174 uses for lookups, and drop ids that contain CTL or space bytes. (2) inbound.cpp deliver_inbound lines 220-241: run the same clean_msgid helper on email.message_id, in_reply_to and references before writing messages.message_id_header, in_reply_to, message_refs and inbound_emails.meta_json. This also covers the Resend headers-map fallback and any other InboundEmail producer. (3) send_internal.cpp payload_from_json: parse with `json::parse_options o; o.allow_invalid_utf8 = true;` (Boost 1.92 is installed) so rows already stored still decode. On any remaining parse failure, throw instead of returning an empty FrozenPayload. load_send_plan/SendAttempt should then mark_failed('payload_corrupt') with Permanent, and cancel_to_draft should throw before it touches message_bodies, so the sent body is never overwritten with ''. (4) As defense in depth, utf8_sanitize every string in payload_to_json.

## [medium] mail-logic/R3: The raw .eml blob (full message including attachments) is never garbage-collected, even after every copy is deleted forever or purged
- File: `backend/src/mail/attachments.cpp`:54 — verdict: confirmed
- Scenario: A user deletes a sensitive mail forever, or it is purged after 30 days in Trash. The attachment blob is GC'd, but the raw .eml with the same content stays in the bucket indefinitely. Storage also grows without bound. Scratch test R3: after delete_forever, is_blob_unreferenced(raw)=false and unreferenced_blobs returns nothing.
- Fix: attachments.cpp kUnreferenced: change the inbound clause so a raw counts as referenced only while (a) some message still has inbound_id = i.id, or (b) the inbound row is not delivered (state pending/failed/unroutable) and younger than a retention window, e.g. `NOT EXISTS (SELECT 1 FROM inbound_emails i WHERE i.raw_sha256 = b.sha256 AND (EXISTS (SELECT 1 FROM messages m WHERE m.inbound_id = i.id) OR (i.state <> 'delivered' AND i.created_at > ?)))`. Keep the pending case so deliver_inbound's register-then-insert inside one tx stays safe. Alternatively, in purge_trash, DeleteForever and user deletion, set inbound_emails.raw_sha256 = NULL when the last message with that inbound_id is gone. Optionally blank html/text/draft in outbound.payload_json for terminal rows that have no copies left.

## [medium] mail-logic/R4: An admin retry of a failed outbound row that was already superseded by a user retry sends the mail a second time
- File: `backend/src/mail/outbound.cpp`:616 — verdict: confirmed
- Scenario: A send fails on quota and alice clicks 重试, which succeeds. Later an admin sees the old row in Outbox → 失败 and clicks retry. The recipient gets the mail twice, and nobody's mailbox shows the second send. Scratch test R4: the admin retry created outbound 3 with 0 copies and a pending send job.
- Fix: outbound.cpp admin_retry_outbound (and defensively clone_failed_outbound): before cloning, require `EXISTS (SELECT 1 FROM messages WHERE outbound_id = ? AND is_draft = 0 AND is_shared_copy = 0)`, otherwise throw invalid_state('该发送记录已被重试或邮件已删除') (409). When cloning, record a 'local.superseded' delivery event (or status_detail) on the old row. Make repo::list_outbox(status='failed') filter with the same EXISTS so superseded rows don't show a Retry button.

## [medium] mail-logic/R5: Spam retention counts from the message's Date header, so old-dated spam is hard-deleted within the hour
- File: `backend/src/mail/mailbox.cpp`:780 — verdict: confirmed
- Scenario: A legitimate but DMARC-failing mail with an old Date (a delayed delivery, resent mail, or mailing-list digest) lands in Spam and is permanently deleted within an hour. Or a user accidentally marks a 2-month-old conversation as spam and loses it at the next purge. Scratch test R5: a spam copy with Date 60 days ago was purged immediately.
- Fix: Add an additive migration with column messages.spam_at INTEGER (index ON messages(spam_at) WHERE is_spam=1). Set it to now in deliver_inbound when inserting with is_spam=1, and in apply_to_thread Spam (`spam_at = ?`). Clear it in NotSpam/Inbox. purge_trash should then use `is_spam = 1 AND COALESCE(spam_at, created_at) <= ?`. A no-migration stopgap is `MAX(date, created_at, updated_at) <= ?`: the Spam action bumps updated_at, and the stopgap can only lengthen retention.

## [medium] mail-logic/R6: A retried send can change body/headers under the same Idempotency-Key: a late-captured parent Message-ID adds In-Reply-To and Resend answers 409, so a delivered mail is marked failed
- File: `backend/src/mail/outbound.cpp`:242 — verdict: confirmed
- Scenario: Alice replies to her own just-sent mail before its Message-ID is known. The POST times out after Resend accepted it. Five seconds later fetch_meta has captured the parent id, and the retry carries In-Reply-To with the same key, so Resend returns 409 and the message shows 发送失败 although it was delivered. If she clicks retry, the recipient gets a duplicate. Scratch test R6: same uuid across attempts, In-Reply-To '' then '<parent-1@resend.dev>'.
- Fix: Freeze the parent at the first attempt that may reach Resend. In SendAttempt::run, after the optional parent GET and before client.send, write the resolved parent id into outbound.payload_json (set in_reply_to, plus `"parent_resolved": true` even when none was found) in a short write tx. In load_send_plan, skip resolve_parent_id when parent_resolved is set, and in SendAttempt skip the parent GET when plan.parent_resolved. That way all retries under the same Idempotency-Key carry byte-identical headers.

## [medium] runtime-wire/RT-3: Retries reuse the Idempotency-Key with a different body (In-Reply-To/References recomputed per attempt), so mail Resend already accepted ends as 'failed'
- File: `backend/src/mail/outbound.cpp`:248 — verdict: confirmed
- Scenario: Reproduced with scratch scenario s91_idem_body_change.py (mock: webhooks_enabled=false per B10, meta_delay=6; fault `{match:'POST /emails', timeout:6}` while RESEND_TIMEOUT_SEC=4). Alice replies to her own just-sent mail. Mock log: POST #1 200 with no In-Reply-To (email created at the mock), POST #2 with the same key and an added In-Reply-To → 409 invalid_idempotent_request. Final status 'failed | 发送请求冲突，请重新发送' while the mock holds 1 email. With fast webhooks the uuid-tag match usually rescues it; with unreachable or late webhooks (a supported deployment mode, B10) it does not.
- Fix: Freeze the wire headers per idempotency key.
- In SendAttempt::run (jobs/outbound_jobs.cpp), before the first client.send for a row, persist plan.headers (In-Reply-To/References) into outbound.payload_json (e.g. a `sent_headers` field) in a short write tx, together with or right after mark_sending.
- In mail::load_send_plan (mail/outbound.cpp), use the frozen `sent_headers` when present instead of calling resolve_parent_id. Skip the B2 parent GET/reload when frozen headers exist.
- Only rows that never reached a POST (no frozen headers) may still pick up a late parent Message-ID.
- Optionally treat a 409 invalid_idempotent_request on attempt > 1 as 'possibly accepted': Retry with backoff and let the webhook or uuid-tag match settle it, instead of mark_failed.
- Regression test: unit test with a fake client that records the bodies of two attempts under the same key, plus an E2E using the mock fault {match:'POST /emails', timeout} with webhooks_enabled=false.

## [medium] runtime-wire/RT-4: POST /emails uses the fixed 30 s overall deadline even for ~37 MB bodies
- File: `backend/src/resend/client.cpp`:194 — verdict: confirmed
- Scenario: Server with an 8 Mbit/s effective path to api.resend.com; a user sends a 25 MiB attachment (accepted by queue_send). Every attempt hits 'request deadline of 30000 ms exceeded while sending request to api.resend.com:443' → Retry. After ~15 h the outbound is marked failed ('发送失败'), having consumed ~370 MB of upstream.
- Fix: In resend/client.cpp:
- Let Impl::call take an optional overall timeout.
- In Client::send (POST /emails), set it to roughly `resend_timeout_sec + body_bytes / (256 KiB/s)`, capped at about 10-15 min.
- Keep ClientOptions::read_timeout (the per-chunk idle limit, re-armed per write_some or read) at resend_timeout_sec, so a stalled connection still fails fast.
- Document the bandwidth assumption in DEPLOY.md and make the rate configurable.
- Add a unit or integration test with a throttled local server: a 10 MB POST at about 1 MB/s with RESEND_TIMEOUT_SEC=4 must succeed.

## [medium] runtime-wire/RT-9: `azmail backup` copies 512 pages per step against a live WAL database; each write by the server restarts the copy
- File: `backend/src/app/support.cpp`:92 — verdict: confirmed
- Scenario: A 5 GB azmail.db (≈1.3 M pages, ≈2,500 steps) with a write every few seconds from webhooks and jobs: every write resets progress, and `azmail backup` does not finish while the server is up.
- Fix: backend/src/app/support.cpp, backup_database(): in WAL mode (the only mode the server uses), copy in a single sqlite3_backup_step(b, -1). It holds just a read snapshot on the source, so writers keep appending to the WAL unblocked. Keep the BUSY/LOCKED retry loop around it.

Alternatively, run `VACUUM INTO '<tmp>'` on the source connection, which also produces a compact, self-contained file.

If stepping is kept, bound the number of restarts (detect with sqlite3_backup_remaining() increasing) and fail with a clear message.

Add a test that runs the backup while a second connection writes in a loop and asserts that it completes.

## [medium] security/SEC-2: Login throttle keeps each attacker-supplied email string (up to 1 MiB) in memory for 15 minutes
- File: `backend/src/http/throttle.cpp`:110 — verdict: confirmed
- Scenario: An unauthenticated attacker POSTs /api/auth/login with {"email":"<1 MB string>N@x","password":"x"}, using a new email each time and a new IPv6 address every 20 attempts. At the scrypt-bound rate of tens of attempts per second, this pins tens of GB within the 15-minute window and the process runs out of memory.
- Fix: In api/auth.cpp auth_login, right after normalize_email: if email.size() > 254 or !is_valid_email(email), skip the user lookup and the throttle map insert. Record the failure for the IP only, still do the dummy scrypt verify (keeps timing uniform), and return 401 invalid_credentials. In http/throttle.cpp, key by_email on a fixed-size digest (crypto::sha256 of the normalized email), cap total entries in by_email and by_ip (for example 100k, evicting the oldest), and key IPv6 addresses by their /64 prefix for the per-IP limit. Also give /api/auth/login its own small body limit in api/routes.cpp, for example 8 KiB instead of J = 1 MiB; a login body needs well under 1 KB.

## [medium] security/SEC-3: A known X-AzMail-Ref turns off the spoofed_internal check for recipients who were not on the original send
- File: `backend/src/mail/inbound.cpp`:286 — verdict: confirmed
- Scenario: An outside party receives one mail from alice@team.com and reads its uuid U from the raw source. They then send a message From: alice@team.com with X-AzMail-Ref: U to bob@team.com, who was not a recipient of U. If the DMARC verdict is not 'fail' (no or relaxed policy, or Resend not reporting it), Bob gets a normal, unflagged message that appears to come from Alice. Because it is not suspicious, remote images also auto-load if Bob trusts Alice.
- Fix: In mail/inbound.cpp deliver_inbound: (a) count a ref match toward spoof clearance only if every local envelope recipient is in the outbound's frozen to/cc/bcc (payload_json), and, when outbound.message_id_header is known, msgid equals it. Otherwise call spam_warnings(..., false) for the non-merge copies. Keep by_ref for choosing the merge path. (b) Call set_outbound_message_id from inbound only when the mail is authenticated (dkim or dmarc 'pass', and never when dmarc=='fail') and the recipient-subset check passes. Better: drop inbound capture source (c) and rely on the webhook and fetch_meta. (c) In outbound.cpp set_outbound_message_id, restrict the late-loopback DELETE to 'in' copies that came from this outbound's own loopback: same from_email as the outbound identity, and inbound_emails.meta_json.x_azmail_ref equal to the uuid or delivered within a short time of accepted_at. (d) If Resend exposes the DKIM d= domain, require it to equal or be a parent of the From domain; otherwise rely on dmarc only.

## [medium] security/SEC-4: Scheduled and retried sends do not re-check send-as permission or account status
- File: `backend/src/mail/outbound.cpp`:299 — verdict: confirmed
- Scenario: An admin removes Bob's can_send_as on support@ or disables Bob's account. Mail Bob scheduled earlier (local or Resend scheduling, up to 30 days ahead) is still sent as support@ or from the disabled account. A send of Bob's from support@ that failed earlier can still be resent as support@ through /api/messages/:id/retry after his permission was revoked.
- Fix: 1) In mail/outbound.cpp mark_sending, inside the same tx, load the sender and check users.disabled = 0 and repo::can_send_as / detail::try_sender(sender_user_id, from_address_id). If either fails, call mark_failed(tx, id, "sender_not_allowed", <zh text>) and return false. Have SendAttempt::run treat false as done. 2) In retry_failed_send and admin_retry_outbound, call resolve_sender(tx.conn(), old.sender_user_id, old.from_address_id) and check the user is active before clone_failed_outbound; otherwise return 403 send_as_forbidden. 3) In repo/accounts.cpp, when an admin disables a user, removes an alias member or clears can_send_as (update_user, alias member update), find that user's outbound rows in queued, sending, accepted or scheduled with scheduled_at > now. Cancel the local ones (status='canceled' plus cancel_to_draft). For scheduled_via='resend' rows with a resend_id, enqueue a job that calls the Resend cancel API after commit. Do the same in delete_user, which currently deletes outbound rows locally but leaves Resend-scheduled mail pending at Resend.

## [medium] security/SEC-5: RFC 2047 decoding is quadratic over the 512 KiB raw header block
- File: `backend/src/mail/eml.cpp`:153 — verdict: confirmed
- Scenario: An external sender sends a stream of emails, each with a 500 KB Subject of repeated '=?x?b?QQ '. Each one occupies an inbound job thread (2 by default) for about 40 s, delaying delivery of all incoming mail for as long as the attacker keeps sending.
- Fix: In mail/eml.cpp parse_encoded_word, limit the terminator search to a window. RFC 2047 §2 caps an encoded-word at 75 characters; use a lenient bound such as 1024 bytes: const auto limit = std::min(s.size(), pos + 1024); find "?=" in s.substr(text_begin, limit - text_begin), and return nullopt if not found. Alternatively, in decode_rfc2047 keep a cached next_close position that is recomputed only once i passes it, which makes the loop linear. Also cap each header value passed to decode_rfc2047 (for example Subject at 16 KiB after unfolding), in eml.cpp extract_headers and in jobs/inbound_jobs.cpp addresses_of and the rcv.subject fallback. Add a test that decoding 512 KB of '=?x?b?QQ ' takes under 100 ms.

## [medium] spec-deploy/F3: Frontend upgrade instructions overwrite the environment's config.js with the build default (apiBase ""), breaking the SPA
- File: `docs/DEPLOY.md`:285 — verdict: confirmed
- Scenario: An operator upgrades the frontend with the documented rsync. config.js now has apiBase "", so the SPA calls https://mail.example.com/api/... on the static origin. nginx-frontend's SPA fallback answers GET /api/auth/me with index.html (200 text/html), which the client reports as invalid_response. POST /api/auth/login gets 405 from the static server. Every user is locked out of the UI until someone rewrites config.js.
- Fix: In docs/DEPLOY.md §10 and §12, use `sudo rsync -a --delete --exclude=/config.js frontend/dist/ /var/www/azmail/`, keep the echo that writes config.js only for the first install, and replace the line-250 wording ("exclude it from --delete") with the --exclude explanation. A more robust alternative: keep the environment's file at /etc/azmail/frontend-config.js and add `alias /etc/azmail/frontend-config.js;` to `location = /config.js` in deploy/nginx-frontend.conf, so no rsync can touch it.

## [low] frontend/F5: Collapsed message rows show no snippet on viewports narrower than 640 px (Gmail-parity bug 2)
- File: `frontend/src/components/mail/MessageItem.tsx`:133 — verdict: confirmed
- Scenario: Open a 3-message thread in a window about 600 px wide: the older collapsed messages read '张三 · 10月7日' with no text, although Message.snippet is non-empty.
- Fix: In MessageItem's collapsed branch, below sm, wrap name+date and the snippet in a `flex min-w-0 flex-1 flex-col sm:flex-row sm:items-center` container, and render the snippet as `block truncate text-sm text-on-surface-variant` (remove `hidden`, keep `sm:flex-1`). Small screens then show it on a second line under the sender.

## [low] frontend/F6: Pager: double-clicking 较旧 pushes the same cursor twice (placeholder data still exposes the previous next_cursor)
- File: `frontend/src/components/mail/ThreadList.tsx`:291 — verdict: confirmed
- Scenario: Reproduced with a scratch vitest test (pager.review.test.tsx): inbox with 120 threads and slow page-2 loading. After two quick clicks on 较旧 the stack is ["C1","C1"], the pager reads '第 101-150 行，共 120 行', and the first row shown is thread 51.
- Fix: ThreadList.tsx: destructure `isPlaceholderData` from useThreadList and set `hasOlder: !!data?.next_cursor && !isPlaceholderData`. In onOlder, guard with `if (data?.next_cursor && !isPlaceholderData && data.next_cursor !== cursor)`. Also make ui.ts pushPage a no-op when the stack's top already equals the cursor.

## [low] frontend/F7: Keyboard shortcuts stop working after clicking inside a message body (iframe takes focus)
- File: `frontend/src/components/mail/EmailFrame.tsx`:106 — verdict: confirmed
- Scenario: Open a thread, click inside the HTML body, then press r, j, e, # or u: nothing happens until the user clicks outside the iframe.
- Fix: In EmailFrame.attach(), add `const stopKeys = frame.contentWindow ? installShortcutListener(frame.contentWindow) : null;` (installShortcutListener accepts any add/removeEventListener target) and call `stopKeys?.()` in cleanupRef. shouldIgnoreKeyEvent's `instanceof Element` checks are false for cross-realm targets, and the frame has no editable content, so this is safe.

## [low] frontend/F8: IME composition keys are not guarded in compose: Esc closes the window, Enter commits half-typed recipients
- File: `frontend/src/components/compose/RecipientField.tsx`:191 — verdict: plausible
- Scenario: In 收件人, type 'lisi' with the Pinyin IME and press Enter to accept the Latin letters: commitText('lisi') creates an invalid red chip mid-composition. In the body, press Esc to dismiss the candidate list: the whole compose window closes (saving and closing).
- Fix: At the top of RecipientField.onKeyDown and ComposeForm.onKeyDown, add `if (e.nativeEvent.isComposing || e.keyCode === 229) return;`. Better, export a small `isImeKeyEvent(e)` helper from lib/keyboard.ts and reuse it in both.

## [low] frontend/F9: Signing out discards unsaved compose edits without flushing or warning
- File: `frontend/src/components/layout/AccountMenu.tsx`:26 — verdict: confirmed
- Scenario: Type a sentence in a compose window and click 退出登录 within about 1.5 s: the draft on the server lacks the sentence, or no draft exists at all for a brand-new message.
- Fix: Before calling logout() in signOut, flush the open savers. Add a module-level registry in useAutosave.ts (Map<winKey, DraftAutosaver>, registered in the mount effect) and an exported `flushAllDrafts(timeoutMs)` that runs Promise.allSettled(saver.flush()) under a timeout. Await it in AccountMenu.signOut. Alternatively, show a ConfirmDialog when any window's saveState is 'dirty'/'saving'/'error'.

## [low] frontend/F10: Messages that arrive in the thread you are viewing are left unread
- File: `frontend/src/components/mail/ThreadView.tsx`:145 — verdict: confirmed
- Scenario: Keep a thread open while the other party replies: the reply appears expanded, but 收件箱 shows 1 unread and the row is bold when you go back.
- Fix: In the expansion effect's fresh-messages branch (or a sibling effect keyed on detail), when any fresh message is non-draft, unread and inViewScope(m, view.folder), and document.visibilityState === 'visible', call performThreadAction(qc, { ids: [threadId], action: 'read' }). To avoid touching other messages, patchMessageOptimistic(qc, threadId, m.id, { is_read: true }) per fresh message also works.

## [low] frontend/F11: Thread list cache key omits the page size, so pages of the old size are served after page_size changes
- File: `frontend/src/components/mail/queries.ts`:75 — verdict: confirmed
- Scenario: While on inbox page 3, change the page size to 25 and press browser Back: the list still shows 50 rows labelled '第 51-75 行'.
- Fix: When the effective page size changes, reset every pager stack and drop the list cache. In ThreadList, add `useEffect` on pageSize: skip the first run, then `useUiStore.setState({ pages: {} })` and `qc.removeQueries({ queryKey: queryKeys.threadsAll() })`. Alternatively do this in GeneralSettings onSuccess and in the settings.changed effect. Optionally make the key unique per size by adding pageSize to the cached filter key in useThreadList, which is additive and leaves the threadsAll() prefix intact.

## [low] frontend/F13: TipTap/ProseMirror is in the main bundle (1.16 MB, 367 KB gzip) although compose is opened on demand
- File: `frontend/src/components/compose/ComposeDock.tsx`:14 — verdict: confirmed
- Scenario: First load over a slow cross-border link: about 370 KB gzip of JS must download and parse before the inbox shows, a large share of it editor code that is not needed until 写邮件. Icons stay blank until the 4 MB font arrives.
- Fix: In ComposeDock.tsx, replace the static import with `const ComposeWindow = lazy(() => import('./ComposeWindow').then(m => ({ default: m.ComposeWindow })))`. Render each window inside <Suspense fallback={<WindowShell-like spinner>}>, keep the ComposeTestOptions type import type-only, and prefetch on idle (requestIdleCallback → import('./ComposeWindow')) so 'c' still opens quickly. Subset the Material Symbols font at build time (DESIGN E1 allows it) and use font-display: swap or a fallback so icons are not blank while the font loads.

## [low] frontend/F14: Label name limits disagree with the server (225 / 100 UTF-16 units vs 64 code points)
- File: `frontend/src/components/mail/LabelDialog.tsx`:18 — verdict: confirmed
- Scenario: Create a label with an 80-character name from the sidebar '+': the client accepts it and the server answers 400.
- Fix: Export one `LABEL_NAME_MAX = 64` (e.g. from lib or settings/validation.ts) and use it in both LabelDialog.tsx and LabelsSettings validation. Count with `Array.from(name.trim()).length`, and set input maxLength to that limit (allowing for surrogate pairs, or validate on submit only). Optionally map invalid_field with details.field === 'name' to the length message.

## [low] frontend/F15: mailto: links in mail bodies put Cc addresses into 收件人 and drop Bcc and body
- File: `frontend/src/components/mail/ThreadView.tsx`:287 — verdict: confirmed
- Scenario: Click `mailto:a@x.com?cc=boss@x.com&body=Hi`: the compose window opens with To: a@x.com, boss@x.com and an empty body.
- Fix: Extend ComposeInit 'new' additively with `cc?: Address[]; bcc?: Address[]; body?: string`. In seedFromInit (seed.ts:51-63), set cc/bcc and prepend `<p>${escapeHtml(body).replace(/\n/g,'<br>')}</p>` to newBody(me). Extend openNewMessage to accept an options object, and pass p.cc, p.bcc and p.body from onMailto. ComposeForm already shows Cc/Bcc when the seed has them.

## [low] frontend/F16: Sent and 已定时 rows show '我' instead of Gmail's '收件人：X'
- File: `frontend/src/components/mail/participants.ts`:50 — verdict: confirmed
- Scenario: Send a mail to 王五 and open 已发送: the row reads '我 · 主题 - 摘要'.
- Fix: Add an additive `to_preview: Address[]` to ThreadListItem: the To/Cc of the newest outbound non-draft message, computed in mailbox.cpp list query or stored by recompute_thread. In participantsView, when folder is 'sent' or 'scheduled', render `收件人：` followed by the shortName list from to_preview (new zh.mail key).

## [low] frontend/F17: Deep link /mail/search/:threadId without ?q= redirects to the inbox instead of opening the thread
- File: `frontend/src/pages/MailPage.tsx`:66 — verdict: confirmed
- Scenario: Visit /mail/search/123: you land on /mail/inbox and thread 123 is not shown.
- Fix: In MailPage, before the search redirect, add `if (kind === 'search' && !q && threadId !== null) return <Navigate to={`/mail/all/${threadId}`} replace />;` (or render ThreadView with folderView('all') as the back target).

## [low] mail-logic/R7: A replayed X-AzMail-Ref with a forged local From skips spoofed_internal, and an uncaptured outbound gets its Message-ID set by the forged mail
- File: `backend/src/mail/inbound.cpp`:299 — verdict: confirmed
- Scenario: ext@ received an alice mail (X-AzMail-Ref: U). ext sends bob a mail with From: alice@team.example, X-AzMail-Ref: U, DKIM fail and no DMARC verdict. Bob's copy is not marked spam, while the same mail without the header is. Scratch test R7: is_spam=0 vs control 1, and the outbound message_id_header became 'forged@evil.example'.
- Fix: inbound.cpp deliver_inbound lines 289-299: count a ref match as verified only when (a) DKIM or DMARC passes, or (b) the delivery's envelope recipients are among the outbound's frozen to/cc/bcc (payload_json) and, if outbound.message_id_header is known, msgid equals it. Pass that `ref_verified` flag to spam_warnings instead of by_ref.has_value(). Call set_outbound_message_id at line 292 only when ref_verified via DKIM/DMARC pass, never from an unauthenticated inbound.

## [low] mail-logic/R8: outbound.reconcile always polls the same 50 oldest rows, so others are never reconciled
- File: `backend/src/mail/outbound.cpp`:288 — verdict: confirmed
- Scenario: With webhooks off and 50 or more mails scheduled days ahead (or stuck in 'sent'), a recently sent mail that was polled once as 'sent' never advances to delivered or bounced in the UI. Scratch test R8: identical batches across rounds; 5 rows never polled again.
- Fix: Add an additive migration with column outbound.last_polled_at. In run_outbound_reconcile, set it after every GET, including NotFound and Duplicate outcomes. outbound_to_reconcile should order by COALESCE(last_polled_at, 0), id. Do not bump updated_at for this: it drives the 7-day window and would keep stuck rows forever.

## [low] mail-logic/R9: A scheduled message keeps its queue time as date, so after it goes out it sorts and displays at the scheduling time
- File: `backend/src/mail/drafts.cpp`:766 — verdict: confirmed
- Scenario: Alice schedules a mail on Monday for Friday. On Friday it is sent, but in 已发送 it is sorted under Monday's position below four days of other mail, and its thread doesn't move up.
- Fix: In outbound.cpp, when a row with scheduled_at actually goes out, update the copies' date to the actual send time: mark_accepted for local scheduling (not sched) and apply_outbound_event on a transition to Sent for scheduled_via=resend. Run `UPDATE messages SET date = ? WHERE outbound_id = ? AND is_draft = 0`. publish_outbound_change already recomputes each copy's thread. Leave immediate sends alone, since their date is already about the send time.

## [low] mail-logic/R10: A lost cancel-schedule response leaves the message stuck as scheduled; Resend last_event 'canceled' is never mapped
- File: `backend/src/mail/types.hpp`:159 — verdict: confirmed
- Scenario: The cancel request to Resend times out but succeeds remotely, and the user sees 502. Every retry then returns 409 'already sent', and the message shows 已定时 in 定时 indefinitely, even though Resend will never send it.
- Fix: (1) When Resend reports last_event 'canceled' (reconcile, fetch_meta, or an email.canceled webhook) for a row in accepted/scheduled with scheduled_via=resend, run the finish_cancel_schedule logic: status 'canceled' plus detail::cancel_to_draft for sender_user_id. Do not just add a status mapping, because a non-draft copy with status 'canceled' falls out of every folder (status_class). (2) In api/mail.cpp messages_cancel_schedule, on a Validation error from cancel, GET the email. If last_event == 'canceled', call finish_cancel_schedule and return the draft instead of 409.

## [low] mail-logic/R11: Split-delivered loopback re-merges into the same copy, making it unread again and emitting a second mail.new
- File: `backend/src/mail/inbound.cpp`:355 — verdict: confirmed
- Scenario: Alice sends as support@ (share_sent) to bob and support@. Resend splits the delivery into two inbound emails. Bob reads the first merge, the second part makes the message unread again, and he gets a second new-mail notification. Scratch test R11: is_read=0 after part 2, mail.new 1→2.
- Fix: inbound.cpp loop-merge branch: also SELECT delivered_to. If it is already non-NULL (an earlier part merged it; sender and shared copies are created with delivered_to NULL, drafts.cpp:602-608, and late-loopback cleanup also sets it), skip the UPDATE, recompute and mail.new emit, and only push the copy into res.copies. Otherwise merge as today.

## [low] mail-logic/R12: threads.attachment_count counts trashed and spam messages, so normal-folder rows show a paperclip with no previews
- File: `backend/src/mail/threads.cpp`:395 — verdict: confirmed
- Scenario: A thread whose only attachment-bearing message was trashed still shows has_attachments=true in Inbox with an empty attachments_preview. Search has:attachment, by contrast, would not match it. Scratch test R12 reproduces this.
- Fix: threads.cpp recompute_thread: add `AND m.trashed_at IS NULL AND m.is_spam = 0` to the attachment_count query, so has_attachments uses the normal set like every other per-view field. Update CONTRACTS.md §H 4 additively. If Spam/Trash views need a paperclip, compute it in decorate() from the scoped preview query (non-empty preview means has_attachments) instead of the thread aggregate.

## [low] runtime-wire/RT-5: Shutdown joins the blocking pools while io threads still run: run_blocking work queued after a join is destroyed after the io_context (use-after-free)
- File: `backend/src/app/app.cpp`:229 — verdict: confirmed
- Scenario: ASan repro of exactly this ordering (/private/tmp/claude-501/review-rt/shutdown_uaf.cpp, header-only Asio 1.92): pool.join() → co_spawn on a strand that holds a tcp socket and co_awaits run_blocking(pool) → ioc.stop()/join → ioc.reset() → pool.reset(). Result: heap-use-after-free in io_context::basic_executor_type::~basic_executor_type, reached from scheduler_operation::destroy in ~thread_pool. In production: `systemctl restart` while a client is slowly uploading, or while a proxy-mode R2 fetch keeps files_workers->join() blocked, crashes during exit.
- Fix: In App::Impl::shutdown (backend/src/app/app.cpp):
(a) After the drain, really close the remaining connections. Add http::Server::close_all(), a close_all_connections(SessionShared&) that posts stream->cancel() or socket close for every handle, not only idle ones, then wait briefly for connections()==0.
(b) Make sessions refuse new run_blocking once a 'pools closing' flag is set: check shared.stopping before step 5a/6 in session.cpp and before the ws_session re-auth, and close instead.
(c) Most important, and enough on its own to remove the UAF: after joining the pools, destroy them before the io_context, i.e. `files_workers.reset(); net_workers.reset(); db_workers.reset();` placed before `server.reset(); ioc.reset();`. Any orphaned co_spawn op is then destroyed while its executor's io_context still exists.

Keep the order drain → close all → runner stop → join pools → stop ioc and join io threads → destroy pools → destroy server → destroy ioc.

## [low] runtime-wire/RT-6: Mock always supplies a complete envelope `received_for` (including Bcc); real Resend derives it from Received-header `for` clauses, so E2E cannot catch dropped recipients
- File: `tools/mock_resend/server.py`:547 — verdict: plausible
- Scenario: An external sender mails To: alice@team with Bcc: dave@team (or To: alice, bob). If Resend's received_for contains only one address from a Received `for` clause, or none, dave (or bob) never receives the mail and nothing is logged as unroutable.
- Fix: First run tools/resend_probe.py (F4.1) against real Resend with a To+Cc+Bcc multi-recipient delivery on the team domain, and record the result in tools/mock_resend/README.md.

Add E2E variants of s09/s10/s23 that inject via POST /_mock/inbound with an explicit partial or empty received_for, so the fallback path is tested. Optionally add a mock config such as `received_for_mode: full|first|none`.

Only if the probe shows partial lists should deliver_inbound change. In that case union in (To ∪ Cc) ∩ local only when received_for is a strict subset of the header recipients and the message passes DMARC/SPF alignment (or comes from a local loopback with a matching X-AzMail-Ref), so the C1 forged-To protection is kept for unauthenticated mail.

## [low] runtime-wire/RT-7: Graceful shutdown can exceed systemd TimeoutStopSec=30; jobs left 'running' then sit until their lease expires after restart
- File: `backend/src/jobs/jobs.cpp`:459 — verdict: confirmed
- Scenario: `systemctl restart azmail` while a slow inbound attachment download is in progress and one browser keeps an upload open → SIGKILL at 30 s. The interrupted outbound.send or inbound.fetch resumes only 5-10 min later.
- Fix: - Budget the phases under TimeoutStopSec: e.g. drain ≤5 s, and runner grace = min(AZMAIL_SHUTDOWN_GRACE_SEC, 20). Or raise TimeoutStopSec in deploy/azmail.service to 45 s and document that it must exceed drain + grace + 5 s.
- Plumb the job stop_token into net::HttpClient (a cancellation flag checked between write_some/read chunks, or a timer cancelling the stream) so transfers abort on stop.
- In Runner::stop, after the grace period, detach or stop joining instead of joining forever.
- On Runner::start, reset 'running' jobs claimed by a previous process: record a per-process run id in jobs.locked_by and recover rows whose locked_by differs, regardless of locked_until.

## [low] runtime-wire/RT-8: inbound.fetch: 15-minute download deadline exceeds the 10-minute lease renewed only before each download
- File: `backend/src/jobs/inbound_jobs.cpp`:250 — verdict: confirmed
- Scenario: A 50 MB attachment fetched at about 70 KB/s (≈12 min) from Resend's CloudFront URL: duplicate concurrent downloads, attempts climb, and the job dies via recovery while admin shows the inbound mail as pending indefinitely.
- Fix: In FetchAttempt (jobs/inbound_jobs.cpp), either keep each download's deadline below the lease, or (better) renew the lease during the transfer. For example, pass a progress callback through resend::Client::download_to → net::HttpRequest (invoked per chunk in exchange()) that calls extend_lease at most every 60 s, and abort the transfer if renewal fails.

Also check the lease (ensure_lease) right before the blob_writer_guard and deliver step.

In jobs::recover_expired_leases, or in the maintenance step, when a job of kind inbound.fetch (or outbound.send) goes 'dead' via lease expiry, record the domain failure: mail::mark_inbound_failed(resend_id, 'lease expired repeatedly') or mark_failed for outbound. Alternatively, let claim() hand an over-limit job to the handler once with `last=true` so its own failure recording runs.

## [low] security/SEC-6: Unauthenticated requests can make the server buffer up to 8 MiB JSON bodies before auth is checked
- File: `backend/src/http/session.cpp`:584 — verdict: confirmed
- Scenario: An attacker opens thousands of connections (max_connections is 4096) and sends PUT /api/drafts/1 with no Authorization header and an 8 MiB body, trickled within the 30 s per-chunk idle timeout. The backend holds about 8 MiB per connection, up to around 32 GiB, before returning 401.
- Fix: In http/session.cpp handle_request, extend step 5a to every route with route.auth == User or Admin and body_pending, not only BodyMode::File. Reuse the same bearer_token + run_blocking(authenticate) block and return 401/403 before reading the body. Optionally pass the Principal on to dispatch to avoid authenticating twice. Count the request toward shared.inflight before reading a body larger than a small threshold such as 64 KiB, or cap total buffered body bytes across connections. Lower the login route's body limit in api/routes.cpp to a few KiB.

## [low] security/SEC-7: A disabled account reveals whether a guessed password is correct
- File: `backend/src/api/auth.cpp`:75 — verdict: confirmed
- Scenario: An attacker guessing passwords for a former employee's disabled account sees 401 for wrong guesses and 403 for the right one, which confirms a password that may be reused elsewhere.
- Fix: Requires a spec change (CONTRACTS.md:240, API.md and the frontend LoginPage). In api/auth.cpp auth_login, when user->disabled, call throttle->record_failure(email, ip), write the audit row with reason 'disabled', and throw the same ApiError::unauthorized("invalid_credentials", ...) as for a wrong password. Alternatively, return account_disabled regardless of the password, by checking user->disabled before or independently of the verify result while still running the verify for timing. That also removes the oracle, but it reveals which accounts are disabled.

## [low] security/SEC-8: Inbound Content-ID and Resend message_id are passed to Resend headers without removing CR/LF
- File: `backend/src/mail/inbound.cpp`:332 — verdict: plausible
- Scenario: An inbound message has a Content-ID that contains CR/LF followed by an extra header line, and Resend's JSON preserves it. When a team member forwards that message with its attachments, the value goes into the Resend send payload as content_id. Whether this injects MIME header lines into the user's outgoing mail depends on whether Resend sanitizes it.
- Fix: Add a strict msg-id sanitizer in mail/types.hpp, either a new sanitize_msg_id or an extension of normalize_message_id: after trimming and stripping <>, return "" if any byte is < 0x21, == 0x7f, or one of '<', '>', '"', '\\', or if the length is over 998. Apply it in resend/client.cpp parse_attachment (content_id), jobs/inbound_jobs.cpp build_inbound_email (rcv.message_id), mail/inbound.cpp (content_id, refs, msgid) and mail/send_internal.cpp format_msgid, so a bad value is dropped and never sent to Resend.

## [low] spec-deploy/F4: DESIGN B7 poll-gap warning is written but never shown; DEPLOY.md says the admin panel shows it
- File: `backend/src/jobs/inbound_jobs.cpp`:383 — verdict: confirmed
- Scenario: The webhook endpoint is broken, for example by a secret mismatch (401) or a firewall, and mail arrives only through polling. The poller detects and backfills the gap but only logs `poll gap detected`. Admins looking at /admin/stats see no warning and never learn that webhook delivery is failing.
- Fix: Add `std::optional<std::string> poll_gap_warning` to repo::AdminStats, filled from db::kv_get(c, kv_keys::kPollGapWarning) in repo/accounts.cpp next to quota_blocked. Serialize it in the admin stats DTO as an additive `poll_gap_warning: string|null` (docs/API.md AdminStats, frontend/src/api/types.ts). Show a warning banner in frontend/src/pages/admin/Stats.tsx next to the quota_blocked banner. Give it a way to clear, for example delete the kv key on a dismiss action or on POST /api/admin/sync, or show it only while its ISO timestamp is less than N days old. Otherwise remove the claim from DEPLOY.md:270 and the 'warns in admin' wording in DESIGN B7.

## [low] spec-deploy/F5: Reschedule is backend-only: the route and client wrapper exist but there is no UI
- File: `frontend/src/api/endpoints.ts`:139 — verdict: confirmed
- Scenario: A user wants to move a scheduled email to a later time. There is no change-time control. The only route is cancel (the message turns back into a draft and the Resend-side schedule is cancelled) and then schedule a new send. A new outbound row and a new Resend email are created, and the in-place PATCH path (`local.rescheduled`/finish_reschedule) is never used.
- Fix: In frontend/src/components/mail/DeliveryStatus.tsx, add a '更改时间' button next to the cancel-schedule button (~line 199) that opens components/compose/SchedulePicker.tsx (initial value message.scheduled_at). On confirm, call reschedule(message.id, ts) and then invalidate queryKeys.thread(message.thread_id), threadsAll() and counts(). Map 409 invalid_state and 502 resend_error to toasts. Add the i18n keys in zh.mail.ts. Otherwise change README.md:8 to '定时发送（可取消）'.

## [low] spec-deploy/F6: Worst-case shutdown (10 s drain + 25 s job grace + pool joins) exceeds systemd TimeoutStopSec=30
- File: `deploy/azmail.service`:24 — verdict: confirmed
- Scenario: `systemctl restart azmail` during an upload streaming to R2 or a slow Resend call. The connection drain uses its 10 s, then a running outbound.send or inbound.fetch uses the 25 s grace. systemd SIGKILLs at 30 s, logs 'Failed with result timeout', and the jobs stay leased until their lease expires, which delays the send or fetch after the restart.
- Fix: Raise TimeoutStopSec in deploy/azmail.service to at least 60 s (10 s drain + AZMAIL_SHUTDOWN_GRACE_SEC + margin) and correct the comments there and in deploy/azmail.env.example:50. In App::Impl::shutdown (app.cpp), measure the time spent draining and pass the remaining budget to the runner, for example via a Runner::stop(std::chrono::milliseconds budget) overload. Bound the join in Runner::stop, or propagate the stop_token into the net HTTP client so outbound.send and inbound.fetch abort promptly. Leases already make abandoning a job safe.

## [low] spec-deploy/F8: nginx error_log records signed-URL query strings (sig) despite the 'never log signed-URL query strings' rule
- File: `deploy/nginx-api.conf`:47 — verdict: confirmed
- Scenario: R2 is slow, so /api/files/ responses time out or the backend restarts mid-download. /var/log/nginx/azmail-api.error.log then contains working signed download links for users' attachments and raw .eml. Anyone with log access (adm group, log shipping) can open them before they expire.
- Fix: In `location /api/files/` of deploy/nginx-api.conf, add `error_log /var/log/nginx/azmail-api.files.error.log crit;` so routine upstream errors on signed URLs are not recorded, or at least send them to a separate root-only log. In DEPLOY.md §9, document that nginx error logs can contain signed links and should be kept 0640 root:adm with short rotation. Leave AZMAIL_SIGNED_URL_TTL_SEC at its default or shorten it.

## [low] spec-deploy/F9: DEPLOY troubleshooting claims the frontend polls every 60 s when the WebSocket is down; only the counts poll
- File: `docs/DEPLOY.md`:304 — verdict: confirmed
- Scenario: /api/ws is misconfigured (wrong Origin or Upgrade). The operator relies on the doc's 'functionality unaffected'. Users keep the inbox open: the unread badge changes but new messages do not appear in the list until they switch tabs or navigate, which looks like lost mail.
- Fix: Change DEPLOY.md:304 to say that while the socket is disconnected only the unread counters poll every 60 s, and mail lists and threads refresh on window focus or navigation. Or, as a behavior fix, add `refetchInterval: socketOpen ? false : 60_000` to useThreadList and the open-thread query in frontend/src/components/mail/queries.ts, using useSocketOpen(), and update DESIGN §5's table to match.

## [low] spec-deploy/F10: DEPLOY.md lists /var/lib/azmail as the default AZMAIL_DATA_DIR/DB_PATH; the code default is relative 'data'
- File: `docs/DEPLOY.md`:134 — verdict: confirmed
- Scenario: An operator trims the env file to the 'required' settings, trusting the documented defaults. The service uses /var/lib/azmail/data/azmail.db. `azm migrate`, `create-user` or `backup` run from /home/admin fail with permission denied creating /home/admin/data; run from /tmp they create and operate on a separate empty database, so the admin user lands in the wrong DB and backups capture the wrong file.
- Fix: Fix the DEPLOY.md:134 row. Either move AZMAIL_DATA_DIR and AZMAIL_DB_PATH into the 'must set' table, or state the default as relative ('data', resolved against the working directory). In the env example, add a comment that these lines must stay. Optionally, have validate_config warn (and doctor fail) when data_dir or db_path is relative and AZMAIL_ALLOW_INSECURE_HTTP=0, which signals production. Also stop doctor from creating the data directory as a side effect.
