/**
 * One typed function per endpoint of docs/API.md. Path ids are numbers; query params go
 * through `buildQuery` (encodeURIComponent). Every GET accepts an optional AbortSignal so it
 * can be used directly as a TanStack Query `queryFn`.
 *
 * Admin endpoints (/api/admin/*) and their query keys live in ./admin.ts [F], not here.
 * Not wrapped here: GET /api/files/* (signed URLs come from the server; use them as links),
 * GET /api/ws (see src/ws), POST /api/webhooks/resend (server-to-server).
 */
import { t } from '@/i18n/zh';
import { api, ApiError, uploadRaw, type UploadProgress } from './client';
import { threadListFilter, type ThreadListFilter } from './queryKeys';
import type {
  Attachment,
  ChangePasswordRequest,
  ContactsResponse,
  Counts,
  Draft,
  DraftInput,
  DraftResponse,
  DraftSendRequest,
  DraftUpdateInput,
  HealthResponse,
  Identity,
  Label,
  LabelInput,
  LabelPatch,
  LoginRequest,
  LoginResponse,
  Me,
  Message,
  MessageEventsResponse,
  MessagePatch,
  SendResult,
  Settings,
  SettingsInput,
  ThreadActionRequest,
  ThreadActionResponse,
  ThreadDetail,
  ThreadListParams,
  ThreadListResponse,
} from './types';

/** Minutes east of UTC, as the API's `tzoff` expects. */
export const currentTzOffset = (): number => -new Date().getTimezoneOffset();

const id = (n: number): string => encodeURIComponent(String(n));

// ───────────── auth ─────────────

/** 401 `invalid_credentials`; 403 `account_disabled`; 429 `too_many_attempts` (`details.retry_after`, s). */
export const login = (body: LoginRequest, signal?: AbortSignal) =>
  api.post<LoginResponse>('/api/auth/login', body, { auth: false, signal });

/** 204; also revokes the session's WebSocket. */
export const logout = () => api.post<void>('/api/auth/logout');

export const getMe = (signal?: AbortSignal) => api.get<Me>('/api/auth/me', { signal });

/** 204; revokes the user's other sessions. 403 `invalid_credentials` (wrong current); 422 `weak_password`. */
export const changePassword = (body: ChangePasswordRequest) => api.post<void>('/api/auth/password', body);

// ───────────── settings / identities ─────────────

export const getSettings = (signal?: AbortSignal) => api.get<Settings>('/api/settings', { signal });

/** Partial update; returns the full settings. */
export const updateSettings = (patch: SettingsInput) => api.put<Settings>('/api/settings', patch);

export const listIdentities = (signal?: AbortSignal) => api.get<Identity[]>('/api/identities', { signal });

// ───────────── threads ─────────────

export const listThreads = (params: ThreadListParams, signal?: AbortSignal) =>
  api.get<ThreadListResponse>('/api/threads', {
    signal,
    query: {
      folder: params.folder,
      label_id: params.label_id,
      q: params.q,
      cursor: params.cursor,
      limit: params.limit,
      tzoff: params.tzoff ?? currentTzOffset(),
    },
  });

/**
 * One page of the list selected by a cache filter (the same value used in
 * `queryKeys.threads(filter, cursor)`), so the request and the key cannot drift apart: the
 * filter is normalized and exactly one of folder / label_id / q is sent.
 */
export function listThreadsFor(
  filter: Partial<ThreadListFilter>,
  cursor: string | null,
  limit: number,
  signal?: AbortSignal,
) {
  const f = threadListFilter(filter);
  return listThreads(
    { folder: f.folder ?? undefined, label_id: f.labelId ?? undefined, q: f.q ?? undefined, cursor, limit },
    signal,
  );
}

/** 404 if the thread does not exist or is not the caller's. */
export const getThread = (threadId: number, signal?: AbortSignal) =>
  api.get<ThreadDetail>(`/api/threads/${id(threadId)}`, { signal });

export const threadActions = (body: ThreadActionRequest) =>
  api.post<ThreadActionResponse>('/api/threads/actions', body);

// ───────────── messages ─────────────

export const getMessage = (messageId: number, signal?: AbortSignal) =>
  api.get<Message>(`/api/messages/${id(messageId)}`, { signal });

export const patchMessage = (messageId: number, patch: MessagePatch) =>
  api.patch<Message>(`/api/messages/${id(messageId)}`, patch);

export const getMessageEvents = (messageId: number, signal?: AbortSignal) =>
  api.get<MessageEventsResponse>(`/api/messages/${id(messageId)}/events`, { signal });

/** Original .eml as text (inbound only). For a download link use `Message.raw_url` instead. */
export const getMessageRaw = (messageId: number, signal?: AbortSignal) =>
  api.get<string>(`/api/messages/${id(messageId)}/raw`, {
    signal,
    responseType: 'text',
    headers: { Accept: 'text/plain' },
  });

/** 409 `too_late` once the undo window has passed. */
export const undoSend = (messageId: number) => api.post<DraftResponse>(`/api/messages/${id(messageId)}/undo-send`);

/** 409 `already_sent`, or `invalid_state` while the scheduling request is in flight; 502 `resend_error`. */
export const cancelSchedule = (messageId: number) =>
  api.post<DraftResponse>(`/api/messages/${id(messageId)}/cancel-schedule`);

/** 422 `invalid_schedule`; 409 `already_sent` / `invalid_state` (as cancel-schedule); 502 `resend_error`. */
export const reschedule = (messageId: number, scheduledAt: number) =>
  api.post<Message>(`/api/messages/${id(messageId)}/reschedule`, { scheduled_at: scheduledAt });

/** Only for status `failed` (else 409 `invalid_state`); creates a new outbound (new idempotency key). 202. */
export const retrySend = (messageId: number) => api.post<SendResult>(`/api/messages/${id(messageId)}/retry`);

// ───────────── drafts ─────────────

export const createDraft = (input: DraftInput) => api.post<Draft>('/api/drafts', input);

export const getDraft = (draftId: number, signal?: AbortSignal) =>
  api.get<Draft>(`/api/drafts/${id(draftId)}`, { signal });

/** 409 `version_conflict` with `details.current: Draft`; pass `force: true` to overwrite. */
export const updateDraft = (draftId: number, input: DraftUpdateInput) =>
  api.put<Draft>(`/api/drafts/${id(draftId)}`, input);

export const deleteDraft = (draftId: number) => api.delete(`/api/drafts/${id(draftId)}`);

/**
 * Saves the final fields and queues the send atomically. 202 `SendResult`.
 * 403 `send_as_forbidden`; 409 `version_conflict` (`details.current: Draft`);
 * 422 `unknown_local_recipient` (`details.emails`) | `too_many_recipients` (`details.field`) |
 * `invalid_schedule` | `no_recipients`; 413 `message_too_large`.
 */
export const sendDraft = (draftId: number, body: DraftSendRequest) =>
  api.post<SendResult>(`/api/drafts/${id(draftId)}/send`, body);

// ───────────── attachments ─────────────

/** Per-file upload limit (the server answers 413 before reading a larger body). */
export const MAX_ATTACHMENT_BYTES = 25 * 1024 * 1024;
/** Total raw attachment bytes per message (send → 413 `message_too_large` above it). */
export const MAX_MESSAGE_ATTACHMENTS_BYTES = 28 * 1024 * 1024;

export interface UploadAttachmentOptions {
  /** Defaults to `File.name`. */
  filename?: string;
  /** Inline images (pasted / dropped into the editor) get a content_id. */
  inline?: boolean;
  onProgress?: (p: UploadProgress) => void;
  signal?: AbortSignal;
}

/**
 * Raw-body upload (≤ 25 MiB) → 201 `Attachment` (unattached until a draft references it).
 * Larger files are rejected locally with 413 `payload_too_large`: the server would refuse them
 * before reading the body, which XHR usually reports as a reset connection (`network_error`).
 * Storage failures: 503 `storage_unavailable` (retryable) / 502 `storage_error`.
 */
export function uploadAttachment(file: Blob, opts: UploadAttachmentOptions = {}): Promise<Attachment> {
  if (file.size > MAX_ATTACHMENT_BYTES) {
    return Promise.reject(
      new ApiError(413, 'payload_too_large', t('errors.payload_too_large'), { limit: MAX_ATTACHMENT_BYTES }),
    );
  }
  const filename = opts.filename ?? (file instanceof File ? file.name : '');
  return uploadRaw<Attachment>('/api/attachments', file, {
    query: { filename: filename || 'attachment', inline: opts.inline ?? false },
    contentType: file.type || 'application/octet-stream',
    onProgress: opts.onProgress,
    signal: opts.signal,
  });
}

// ───────────── labels / counts / contacts ─────────────

export const listLabels = (signal?: AbortSignal) => api.get<Label[]>('/api/labels', { signal });

export const getLabel = (labelId: number, signal?: AbortSignal) =>
  api.get<Label>(`/api/labels/${id(labelId)}`, { signal });

export const createLabel = (input: LabelInput) => api.post<Label>('/api/labels', input);

export const updateLabel = (labelId: number, patch: LabelPatch) =>
  api.patch<Label>(`/api/labels/${id(labelId)}`, patch);

export const deleteLabel = (labelId: number) => api.delete(`/api/labels/${id(labelId)}`);

export const getCounts = (signal?: AbortSignal) => api.get<Counts>('/api/counts', { signal });

export const searchContacts = (q: string, limit = 8, signal?: AbortSignal) =>
  api.get<ContactsResponse>('/api/contacts', { signal, query: { q, limit } });

// ───────────── health ─────────────

export const getHealth = (signal?: AbortSignal) => api.get<HealthResponse>('/api/health', { signal, auth: false });
