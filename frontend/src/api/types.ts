/**
 * Wire types for the AZ Mail REST / WebSocket API.
 *
 * The "Main shapes" section mirrors docs/API.md EXACTLY (field names, nullability, unions).
 * Everything after it types the remaining requests/responses of the API table. Where API.md
 * leaves a shape open, the choice made here is the contract ("API.md Addendum B"; backend serde
 * — WP-B `mail/serde`, WP-D `api/*`, WP-A `ws::Hub` — must match):
 *
 *   General
 *   - DELETE routes return 204 with an empty body (typed `void`); so do logout and password.
 *   - Unpaged collection GETs return bare arrays: GET /api/labels → Label[],
 *     /api/identities → Identity[], /api/admin/users → AdminUser[], /admin/aliases → AdminAlias[],
 *     /admin/domains → DomainRow[], /admin/inbound → InboundRow[], /admin/outbox → OutboxRow[],
 *     /admin/jobs → JobRow[].
 *   - Cursor-paged lists return `{ items, next_cursor }`: GET /api/admin/events → CursorPage<WebhookEventRow>.
 *   - TEXT JSON columns are sent as parsed JSON values, never as JSON-encoded strings:
 *     InboundRow.recipients (recipients_json) is string[], JobRow.payload is an object,
 *     DeliveryEvent.detail (detail_json) is an object (`{}` when empty).
 *   - Error specifics live in `error.details`: 429 too_many_attempts → `details.retry_after`
 *     (seconds); 409 version_conflict → `details.current` (Draft; PUT /api/drafts/:id and
 *     POST /api/drafts/:id/send); 400 invalid_field → `details.field`;
 *     422 unknown_local_recipient → `details.emails`; 422 too_many_recipients → `details.field`.
 *   - POST /api/auth/password with a wrong current_password → 403 `invalid_credentials`
 *     (not 401: a 401 on an authenticated request ends the client session).
 *   - Draft.mode is always present ('new' by default), never null.
 *
 *   Route → response (where API.md is silent)
 *   - PUT /api/settings → the full Settings after applying the partial update.
 *   - GET /api/labels/:id → Label; POST /api/labels → 201 Label; PATCH → Label; DELETE → 204.
 *   - GET /api/messages/:id/events → { events: DeliveryEvent[] }.
 *   - POST /api/admin/users → 201 AdminUser; PATCH /api/admin/users/:id → AdminUser; DELETE → 204.
 *   - POST /api/admin/aliases → 201 AdminAlias; PATCH /api/admin/aliases/:id → AdminAlias; DELETE → 204.
 *   - GET /api/admin/domains/:id → DomainRow; POST → 201 DomainRow; DELETE → 204;
 *     GET /api/admin/domains/:id/status → DomainStatus.
 *   - POST /api/admin/outbox/:id/retry → the NEW OutboxRow (new id and uuid; the original row
 *     stays `failed`), so callers invalidate ['admin','outbox'] instead of patching by id.
 *   - POST /api/admin/jobs/:id/retry → JobRow (the row after re-queueing).
 *   - POST /api/admin/sync → 202 { job_id }.
 *
 *   WebSocket
 *   - Server frames are FLAT: the Hub serializes the `data` object's fields beside `type`
 *     (`{type, ...data}`), never `{type, data:{…}}`. Events without contents are `{type}`.
 *   - `mail.new.from` is an Address `{name, email}`; `outbound.status.status_detail` is string|null.
 *
 * All ids are numbers and all times are ms-epoch UTC integers.
 */

// ───────────────────────────── Main shapes (docs/API.md) ─────────────────────────────

export type Address = { name: string; email: string };

export type OutboundStatus =
  | 'queued'
  | 'sending'
  | 'accepted'
  | 'scheduled'
  | 'sent'
  | 'delivered'
  | 'delivery_delayed'
  | 'bounced'
  | 'complained'
  | 'failed'
  | 'suppressed'
  | 'canceled';

export interface Me {
  id: number;
  email: string;
  display_name: string;
  is_admin: boolean;
  settings: Settings;
  identities: Identity[];
  /** Addendum A: origins serving file bytes (API origin + R2 public origins) for the email iframe CSP. */
  server: { files_origins: string[]; blob_backend: 'local' | 'r2'; version: string };
}

export interface Identity {
  address_id: number;
  email: string;
  display_name: string;
  kind: 'user' | 'alias';
  is_default: boolean;
}

export interface Settings {
  undo_send_seconds: 0 | 5 | 10 | 20 | 30;
  signature_html: string;
  signature_enabled: boolean;
  timezone: string;
  page_size: number;
  remote_images: 'ask' | 'always';
  trusted_image_senders: string[];
  display_name: string;
}

export interface ThreadListItem {
  id: number;
  subject: string;
  snippet: string;
  participants: { name: string; email: string; is_me: boolean; unread: boolean }[];
  message_count: number;
  draft_count: number;
  unread: boolean;
  starred: boolean;
  has_attachments: boolean;
  label_ids: number[];
  last_at: number;
  in_inbox: boolean;
  latest_status: OutboundStatus | null;
  scheduled_at: number | null;
  /** At most 3. */
  attachments_preview: { id: number; filename: string; content_type: string }[];
}

/** `total` is null for search results. */
export interface ThreadListResponse {
  items: ThreadListItem[];
  next_cursor: string | null;
  total: number | null;
}

export interface Attachment {
  id: number;
  filename: string;
  content_type: string;
  size: number;
  inline: boolean;
  content_id: string | null;
  download_url: string;
  view_url: string | null;
}

export interface Message {
  id: number;
  thread_id: number;
  direction: 'in' | 'out';
  is_draft: boolean;
  from: Address;
  /** Shared alias copy: the actual sender. */
  sent_by: Address | null;
  to: Address[];
  cc: Address[];
  bcc: Address[];
  reply_to: Address[];
  delivered_to: string | null;
  subject: string;
  snippet: string;
  date: number;
  /** `cid:` references are already rewritten to signed URLs. */
  html: string | null;
  text: string | null;
  attachments: Attachment[];
  is_read: boolean;
  is_starred: boolean;
  in_inbox: boolean;
  is_spam: boolean;
  trashed: boolean;
  label_ids: number[];
  auth: { spf: string | null; dkim: string | null; dmarc: string | null } | null;
  warnings: ('dmarc_fail' | 'spoofed_internal')[];
  outbound: {
    id: number;
    status: OutboundStatus;
    status_detail: string | null;
    scheduled_at: number | null;
    scheduled_via: 'resend' | 'local' | null;
    undo_until: number | null;
    sent_at: number | null;
  } | null;
  message_id_header: string | null;
  raw_url: string | null;
}

/** Includes drafts and trashed messages; the UI filters. */
export interface ThreadDetail {
  id: number;
  subject: string;
  label_ids: number[];
  messages: Message[];
}

/** Additive name for the draft mode union of API.md. */
export type DraftMode = 'new' | 'reply' | 'reply_all' | 'forward';

export interface DraftInput {
  mode?: DraftMode;
  parent_message_id?: number | null;
  from_address_id?: number;
  to?: Address[];
  cc?: Address[];
  bcc?: Address[];
  subject?: string;
  html?: string;
  quoted_html?: string | null;
  attachment_ids?: number[];
  include_parent_attachments?: boolean;
}

export interface Draft {
  id: number;
  thread_id: number;
  version: number;
  /** Always present ('new' by default), never null. */
  mode: DraftMode;
  parent_message_id: number | null;
  from_address_id: number;
  to: Address[];
  cc: Address[];
  bcc: Address[];
  subject: string;
  /** `cid:` references are already rewritten to signed URLs (as in Message.html). */
  html: string;
  /** Kept outside the editor; signed URLs like `html` (the server stores both with `cid:`). */
  quoted_html: string | null;
  attachments: Attachment[];
  updated_at: number;
}

export interface SendResult {
  message_id: number;
  thread_id: number;
  outbound_id: number;
  status: OutboundStatus;
  undo_ms: number;
  scheduled_at: number | null;
}

export interface Label {
  id: number;
  name: string;
  color: string;
  sort_order: number;
}

export interface AdminUser {
  id: number;
  email: string;
  display_name: string;
  is_admin: boolean;
  disabled: boolean;
  created_at: number;
  last_login_at: number | null;
  message_count: number;
  storage_bytes: number;
  aliases: { id: number; email: string; can_send_as: boolean }[];
}

export interface AdminAlias {
  id: number;
  email: string;
  display_name: string;
  share_sent: boolean;
  created_at: number;
  members: { user_id: number; email: string; display_name: string; can_send_as: boolean }[];
}

// ───────────────────────────── Errors ─────────────────────────────

/**
 * Error codes the UI may branch on. The server may send others; `(string & {})` keeps
 * autocompletion. Sources: the `ApiError` factory defaults in backend/src/core/errors.hpp,
 * backend/src/core/json.cpp (invalid_json / invalid_field), the route-specific codes in API.md,
 * and the backend additions marked ★ in docs/CONTRACTS.md §E.
 */
export type KnownErrorCode =
  // errors.hpp factory defaults (status in brackets)
  | 'bad_request' // 400
  | 'unauthorized' // 401
  | 'forbidden' // 403
  | 'not_found' // 404
  | 'conflict' // 409
  | 'payload_too_large' // 413 (upload > 25 MiB, body over the route limit)
  | 'unprocessable' // 422
  | 'too_many_requests' // 429
  | 'internal_error' // 500
  | 'bad_gateway' // 502
  | 'service_unavailable' // 503 (server in-flight cap)
  // core/json
  | 'invalid_json' // 400
  | 'invalid_field' // 400, details.field
  // route-specific (API.md)
  | 'invalid_credentials' // 401 login; 403 POST /api/auth/password
  | 'too_many_attempts' // 429, details.retry_after
  | 'version_conflict' // 409, details.current
  | 'too_late' // 409 undo-send
  | 'already_sent' // 409 cancel-schedule
  | 'resend_error' // 502 cancel-schedule / reschedule
  | 'send_as_forbidden' // 403 send
  | 'unknown_local_recipient' // 422 send
  | 'too_many_recipients' // 422 send
  | 'invalid_schedule' // 422 send
  | 'message_too_large' // 413 send
  // backend additions (docs/CONTRACTS.md §E ★)
  | 'invalid_signature' // 403 bad/expired signed /api/files URL (401 only on the webhook)
  | 'account_disabled' // 403 login of a disabled user
  | 'method_not_allowed' // 405
  | 'invalid_state' // 409 retry / reschedule / cancel-schedule / admin retry in the wrong state
  | 'address_exists' // 409 admin user / alias create or update
  | 'domain_exists' // 409 admin domain create
  | 'label_exists' // 409 label create / update (name, case-insensitive)
  | 'last_admin' // 409 admin user update / delete
  | 'cannot_delete_self' // 409 admin user delete
  | 'domain_in_use' // 409 admin domain delete
  | 'alias_in_use' // 409 admin alias delete (outbound mail references it; remove its members instead)
  | 'no_recipients' // 422 send
  | 'unknown_domain' // 422 admin user / alias create
  | 'weak_password' // 422 password change, admin user create / patch
  | 'storage_error' // 502 upload / files (non-retryable blob store error)
  | 'storage_unavailable' // 503 upload / files (retryable blob store error)
  // client-side only
  | 'network_error'
  | 'invalid_response';

export type ApiErrorCode = KnownErrorCode | (string & {});

/** `{"error":{"code":"snake_case","message":"中文说明","details":{}}}` */
export interface ApiErrorBody {
  error: { code: ApiErrorCode; message: string; details?: Record<string, unknown> };
}

/** details of 429 `too_many_attempts`. */
export interface TooManyAttemptsDetails {
  /** Seconds. */
  retry_after: number;
}

/** details of 409 `version_conflict` (PUT /api/drafts/:id and POST /api/drafts/:id/send). */
export interface VersionConflictDetails {
  current: Draft;
}

/** details of 400 `invalid_field`; nested fields use dotted paths ("draft.to"). */
export interface InvalidFieldDetails {
  field: string;
}

/** details of 422 `unknown_local_recipient` (POST /api/drafts/:id/send). */
export interface UnknownLocalRecipientDetails {
  /** Local-domain recipients that have no mailbox or alias. */
  emails: string[];
}

/** details of 422 `too_many_recipients` (POST /api/drafts/:id/send): the field over the limit. */
export interface TooManyRecipientsDetails {
  field: string;
}

// ───────────────────────────── Auth / settings ─────────────────────────────

export interface LoginRequest {
  email: string;
  password: string;
}

export interface LoginResponse {
  token: string;
  expires_at: number;
  user: Me;
}

export interface ChangePasswordRequest {
  current_password: string;
  new_password: string;
}

/** PUT /api/settings is a partial update. */
export type SettingsInput = Partial<Settings>;

// ───────────────────────────── Threads / messages ─────────────────────────────

export type FolderId = 'inbox' | 'starred' | 'scheduled' | 'sent' | 'drafts' | 'all' | 'spam' | 'trash';

export const FOLDER_IDS: readonly FolderId[] = [
  'inbox',
  'starred',
  'scheduled',
  'sent',
  'drafts',
  'all',
  'spam',
  'trash',
] as const;

/** GET /api/threads — exactly one of folder / label_id / q selects the view. */
export interface ThreadListParams {
  folder?: FolderId;
  label_id?: number;
  q?: string;
  cursor?: string | null;
  limit?: number;
  /** Minutes east of UTC (`-new Date().getTimezoneOffset()`); filled in by the endpoint helper. */
  tzoff?: number;
}

export type ThreadAction =
  | 'archive'
  | 'inbox'
  | 'read'
  | 'unread'
  | 'star'
  | 'unstar'
  | 'trash'
  | 'restore'
  | 'spam'
  | 'not_spam'
  | 'delete_forever'
  | 'add_label'
  | 'remove_label';

export type LabelThreadAction = Extract<ThreadAction, 'add_label' | 'remove_label'>;

/** POST /api/threads/actions — `label_id` is required for add_label / remove_label, absent otherwise. */
export type ThreadActionRequest =
  | { thread_ids: number[]; action: LabelThreadAction; label_id: number }
  | { thread_ids: number[]; action: Exclude<ThreadAction, LabelThreadAction>; label_id?: never };

export interface ThreadActionResponse {
  thread_ids: number[];
}

export interface MessagePatch {
  is_read?: boolean;
  is_starred?: boolean;
  add_label_ids?: number[];
  remove_label_ids?: number[];
}

/** Known `delivery_events.type` values; Resend `email.*` events plus local ones. */
export type DeliveryEventType =
  | 'email.sent'
  | 'email.scheduled'
  | 'email.delivered'
  | 'email.delivery_delayed'
  | 'email.bounced'
  | 'email.complained'
  | 'email.failed'
  | 'email.suppressed'
  | 'email.opened'
  | 'email.clicked'
  | 'local.queued'
  | 'local.accepted'
  | 'local.failed'
  | 'local.canceled'
  | 'local.rescheduled'
  | (string & {});

export interface DeliveryEvent {
  type: DeliveryEventType;
  occurred_at: number;
  /** Event specifics, e.g. bounce text; `{}` when none. */
  detail: Record<string, unknown>;
}

/** GET /api/messages/:id/events */
export interface MessageEventsResponse {
  events: DeliveryEvent[];
}

/** POST /api/messages/:id/undo-send and /cancel-schedule */
export interface DraftResponse {
  draft: Draft;
}

/** POST /api/messages/:id/reschedule */
export interface RescheduleRequest {
  scheduled_at: number;
}

// ───────────────────────────── Drafts ─────────────────────────────

/** PUT /api/drafts/:id */
export type DraftUpdateInput = DraftInput & { version: number; force?: boolean };

/** POST /api/drafts/:id/send — `draft` carries the final field values (saved atomically). */
export interface DraftSendRequest {
  version: number;
  draft?: DraftInput;
  /** null/absent = send now (after the undo window). */
  scheduled_at?: number | null;
}

// ───────────────────────────── Attachments ─────────────────────────────

/** POST /api/attachments?filename=&inline= (raw body, Content-Type = file type) */
export interface UploadAttachmentParams {
  filename: string;
  inline: boolean;
}

// ───────────────────────────── Labels / counts / contacts ─────────────────────────────

export interface LabelInput {
  name: string;
  color: string;
  sort_order?: number;
}

export type LabelPatch = Partial<LabelInput>;

export interface LabelCount {
  unread: number;
  total: number;
}

export interface Counts {
  inbox_unread: number;
  drafts: number;
  scheduled: number;
  spam_unread: number;
  /** Keyed by label id (JSON object keys are strings). */
  labels: Record<string, LabelCount>;
}

export type ContactKind = 'team' | 'alias' | 'contact';

export interface Contact {
  name: string;
  email: string;
  kind: ContactKind;
}

export interface ContactsResponse {
  items: Contact[];
}

// ───────────────────────────── Health ─────────────────────────────

export interface HealthResponse {
  status: string;
  version: string;
  db: string;
  time: number;
}

// ───────────────────────────── Admin ─────────────────────────────

export interface CursorPage<T> {
  items: T[];
  next_cursor: string | null;
}

export interface AdminUserCreate {
  email: string;
  display_name: string;
  password: string;
  is_admin?: boolean;
}

export interface AdminUserPatch {
  display_name?: string;
  is_admin?: boolean;
  disabled?: boolean;
  password?: string;
}

export interface AdminAliasMemberInput {
  user_id: number;
  can_send_as: boolean;
}

export interface AdminAliasInput {
  email: string;
  display_name: string;
  share_sent: boolean;
  members: AdminAliasMemberInput[];
}

/** PATCH /api/admin/aliases/:id — `members`, when present, replaces the list wholesale. */
export type AdminAliasPatch = Partial<AdminAliasInput>;

export interface DomainInput {
  name: string;
}

export interface DomainRow {
  id: number;
  name: string;
  receiving_enabled: boolean;
  created_at: number;
}

/** Resend domain verification states. */
export type ResendDomainState = 'not_started' | 'pending' | 'verified' | 'failed' | 'temporary_failure';

export interface DomainDnsRecord {
  record: string;
  name: string;
  type: string;
  ttl: string;
  status: string;
  value: string;
  priority?: number;
}

/** GET /api/admin/domains/:id/status — proxied from Resend `GET /domains`; `resend` is null if Resend doesn't know it. */
export interface DomainStatus {
  id: number;
  name: string;
  resend: {
    id: string;
    status: ResendDomainState | (string & {});
    region: string | null;
    created_at: number | null;
    records: DomainDnsRecord[];
  } | null;
}

/** GET /api/admin/events — metadata only (no payload: admins never see mail bodies). */
export interface WebhookEventRow {
  id: number;
  svix_id: string;
  type: string;
  resend_email_id: string | null;
  received_at: number;
  processed_at: number | null;
  /** applied | enqueued | ignored_unknown | error:… */
  result: string | null;
}

export interface AdminEventsParams {
  type?: string;
  cursor?: string | null;
}

export type InboundState = 'pending' | 'delivered' | 'unroutable' | 'failed';

export interface InboundRow {
  id: number;
  resend_id: string;
  state: InboundState;
  source: 'webhook' | 'poll' | 'admin';
  message_id_header: string | null;
  from_email: string | null;
  subject: string | null;
  received_at: number | null;
  /** Envelope recipients. */
  recipients: string[];
  error: string | null;
  created_at: number;
  updated_at: number;
}

export interface OutboxRow {
  id: number;
  uuid: string;
  sender_user_id: number;
  sender_email: string;
  from_email: string;
  status: OutboundStatus;
  status_detail: string | null;
  error_name: string | null;
  scheduled_at: number | null;
  scheduled_via: 'resend' | 'local' | null;
  resend_id: string | null;
  total_bytes: number;
  last_event: string | null;
  last_event_at: number | null;
  created_at: number;
  updated_at: number;
}

export type JobState = 'pending' | 'running' | 'done' | 'dead' | 'canceled';

export interface JobRow {
  id: number;
  kind: string;
  lane: 'outbound' | 'inbound' | 'sync' | 'maintenance' | (string & {});
  priority: number;
  payload: Record<string, unknown>;
  state: JobState;
  run_at: number;
  attempts: number;
  max_attempts: number;
  locked_until: number | null;
  dedupe_key: string | null;
  last_error: string | null;
  created_at: number;
  updated_at: number;
}

/** POST /api/admin/sync → 202 */
export interface SyncResponse {
  job_id: number;
}

export interface AdminStats {
  users: number;
  messages: number;
  storage_bytes: number;
  queue: { pending: number; dead: number };
  sent_24h: number;
  received_24h: number;
  failed_24h: number;
  last_webhook_at: number | null;
  last_poll_at: number | null;
  quota_blocked: boolean;
  /** Addendum A. */
  storage: {
    backend: 'local' | 'r2';
    delivery: 'redirect' | 'proxy';
    blob_count: number;
    blob_bytes: number;
  };
}

// ───────────────────────────── WebSocket protocol ─────────────────────────────

// Frames are flat JSON objects: `{"type":"mail.new","thread_id":1,…}` — the Hub writes the fields of
// `Notifier::publish(user_id, type, data)`'s `data` beside `type` (no nested `data` key).

/**
 * Close code: auth failed or timed out. Also follows a `session.revoked` frame (logout, password
 * change or admin disable elsewhere, or the server's 5-minute re-authentication failing).
 */
export const WS_CLOSE_AUTH_FAILED = 4401;

export type WsClientMessage = { type: 'auth'; token: string } | { type: 'ping' };

export interface WsReadyEvent {
  type: 'ready';
  user_id: number;
  server_time: number;
}
export interface WsPongEvent {
  type: 'pong';
}
export interface WsMailNewEvent {
  type: 'mail.new';
  thread_id: number;
  message_id: number;
  from: Address;
  subject: string;
  snippet: string;
  in_inbox: boolean;
  is_spam: boolean;
}
export interface WsThreadsChangedEvent {
  type: 'threads.changed';
  thread_ids: number[];
}
export interface WsOutboundStatusEvent {
  type: 'outbound.status';
  message_id: number;
  thread_id: number;
  outbound_id: number;
  status: OutboundStatus;
  status_detail: string | null;
}
export interface WsLabelsChangedEvent {
  type: 'labels.changed';
}
export interface WsSettingsChangedEvent {
  type: 'settings.changed';
}
export interface WsSessionRevokedEvent {
  type: 'session.revoked';
}

/** Server → client messages (discriminated on `type`). Events are invalidation hints, not data. */
export type WsServerEvent =
  | WsReadyEvent
  | WsPongEvent
  | WsMailNewEvent
  | WsThreadsChangedEvent
  | WsOutboundStatusEvent
  | WsLabelsChangedEvent
  | WsSettingsChangedEvent
  | WsSessionRevokedEvent;

export type WsServerEventType = WsServerEvent['type'];
