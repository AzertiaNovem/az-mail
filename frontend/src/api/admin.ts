/**
 * Admin endpoints (/api/admin/*, admin-only) and their query keys [WP-F after WP0].
 *
 * Shapes follow api/types.ts ("API.md Addendum B"): collection GETs return bare arrays, events
 * are cursor-paged `{items, next_cursor}`, DELETE → 204, sync → 202 `{job_id}`, retries return
 * the resulting row (outbox retry: a NEW row). Every GET accepts an AbortSignal so it can be a
 * TanStack Query `queryFn`.
 *
 * Keys all start with 'admin' (staleTime 0, `staleTimes.admin`), so admin mutations can
 * invalidate everything with `invalidateQueries({ queryKey: adminKeys.all() })`.
 */
import { api } from './client';
import type {
  AdminAlias,
  AdminAliasInput,
  AdminAliasPatch,
  AdminEventsParams,
  AdminStats,
  AdminUser,
  AdminUserCreate,
  AdminUserPatch,
  CursorPage,
  DomainInput,
  DomainRow,
  DomainStatus,
  InboundRow,
  InboundState,
  JobRow,
  JobState,
  OutboundStatus,
  OutboxRow,
  SyncResponse,
  WebhookEventRow,
} from './types';

const id = (n: number): string => encodeURIComponent(String(n));

/** `?state=` filter of GET /api/admin/inbound. */
export type AdminInboundFilter = Extract<InboundState, 'unroutable' | 'failed'>;
/** `?status=` filter of GET /api/admin/outbox. */
export type AdminOutboxFilter = Extract<OutboundStatus, 'failed' | 'queued' | 'sending'>;
/** `?state=` filter of GET /api/admin/jobs. */
export type AdminJobsFilter = Extract<JobState, 'dead'>;

// ───────────── query keys ─────────────

export const adminKeys = {
  /** Prefix of every admin query. */
  all: () => ['admin'] as const,
  users: () => ['admin', 'users'] as const,
  aliases: () => ['admin', 'aliases'] as const,
  /** Prefix of the domain list and every per-domain key. */
  domains: () => ['admin', 'domains'] as const,
  domain: (domainId: number) => ['admin', 'domains', domainId] as const,
  domainStatus: (domainId: number) => ['admin', 'domains', domainId, 'status'] as const,
  events: (type: string | null = null, cursor: string | null = null) =>
    ['admin', 'events', { type: type || null }, cursor] as const,
  inbound: (state: AdminInboundFilter | null = null) => ['admin', 'inbound', state] as const,
  outbox: (status: AdminOutboxFilter | null = null) => ['admin', 'outbox', status] as const,
  jobs: (state: AdminJobsFilter | null = null) => ['admin', 'jobs', state] as const,
  stats: () => ['admin', 'stats'] as const,
} as const;

// ───────────── users ─────────────

export const adminListUsers = (signal?: AbortSignal) => api.get<AdminUser[]>('/api/admin/users', { signal });

/** 201; the address's domain must exist (422 `unknown_domain`). 409 `address_exists`; 422 `weak_password`. */
export const adminCreateUser = (body: AdminUserCreate) => api.post<AdminUser>('/api/admin/users', body);

/** 409 `last_admin`; 422 `weak_password` (when `password` is set). */
export const adminUpdateUser = (userId: number, patch: AdminUserPatch) =>
  api.patch<AdminUser>(`/api/admin/users/${id(userId)}`, patch);

/** 204; 409 `cannot_delete_self` / `last_admin`. */
export const adminDeleteUser = (userId: number) => api.delete(`/api/admin/users/${id(userId)}`);

// ───────────── aliases ─────────────

export const adminListAliases = (signal?: AbortSignal) => api.get<AdminAlias[]>('/api/admin/aliases', { signal });

/** 201. 409 `address_exists`; 422 `unknown_domain`. */
export const adminCreateAlias = (body: AdminAliasInput) => api.post<AdminAlias>('/api/admin/aliases', body);

/** `members`, when given, replaces the list wholesale. */
export const adminUpdateAlias = (aliasId: number, patch: AdminAliasPatch) =>
  api.patch<AdminAlias>(`/api/admin/aliases/${id(aliasId)}`, patch);

/** 204; 409 `alias_in_use` when sent mail references the alias (remove its members instead). */
export const adminDeleteAlias = (aliasId: number) => api.delete(`/api/admin/aliases/${id(aliasId)}`);

// ───────────── domains ─────────────

export const adminListDomains = (signal?: AbortSignal) => api.get<DomainRow[]>('/api/admin/domains', { signal });

export const adminGetDomain = (domainId: number, signal?: AbortSignal) =>
  api.get<DomainRow>(`/api/admin/domains/${id(domainId)}`, { signal });

/** 201; 409 `domain_exists`. */
export const adminCreateDomain = (body: DomainInput) => api.post<DomainRow>('/api/admin/domains', body);

/** 204; 409 `domain_in_use` while addresses use it. */
export const adminDeleteDomain = (domainId: number) => api.delete(`/api/admin/domains/${id(domainId)}`);

/** Proxies Resend `GET /domains` (slow; net pool). */
export const adminGetDomainStatus = (domainId: number, signal?: AbortSignal) =>
  api.get<DomainStatus>(`/api/admin/domains/${id(domainId)}/status`, { signal });

// ───────────── webhook events / inbound / outbox / jobs ─────────────

export const adminListEvents = (params: AdminEventsParams = {}, signal?: AbortSignal) =>
  api.get<CursorPage<WebhookEventRow>>('/api/admin/events', {
    signal,
    query: { type: params.type, cursor: params.cursor },
  });

export const adminListInbound = (state?: AdminInboundFilter, signal?: AbortSignal) =>
  api.get<InboundRow[]>('/api/admin/inbound', { signal, query: { state } });

export const adminListOutbox = (status?: AdminOutboxFilter, signal?: AbortSignal) =>
  api.get<OutboxRow[]>('/api/admin/outbox', { signal, query: { status } });

/**
 * Re-sends a failed outbound as a NEW outbound and returns that NEW OutboxRow (new id and uuid;
 * the original row stays `failed`). Invalidate the `['admin', 'outbox']` prefix (every
 * status filter) rather than patching by id.
 * 409 `invalid_state` when the row is not failed.
 */
export const adminRetryOutbox = (outboundId: number) =>
  api.post<OutboxRow>(`/api/admin/outbox/${id(outboundId)}/retry`);

export const adminListJobs = (state?: AdminJobsFilter, signal?: AbortSignal) =>
  api.get<JobRow[]>('/api/admin/jobs', { signal, query: { state } });

/** Re-queues a dead job; returns the same row after re-queueing. 409 `invalid_state` when not dead. */
export const adminRetryJob = (jobId: number) => api.post<JobRow>(`/api/admin/jobs/${id(jobId)}/retry`);

/** Enqueues `poll.receiving`; 202 `{job_id}`. */
export const adminSync = () => api.post<SyncResponse>('/api/admin/sync');

// ───────────── stats ─────────────

export const adminGetStats = (signal?: AbortSignal) => api.get<AdminStats>('/api/admin/stats', { signal });
