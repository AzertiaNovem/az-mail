/**
 * TanStack Query key factories and stale times (DESIGN.md §5 "Query keys and invalidation").
 *
 * | Key                                      | staleTime                                   |
 * |------------------------------------------|---------------------------------------------|
 * | ['me']                                   | 5 min                                       |
 * | ['labels']                               | 5 min                                       |
 * | ['counts']                               | 15 s; refetchInterval 60 s while WS is down |
 * | ['threads', {folder,labelId,q}, cursor]  | 30 s                                        |
 * | ['thread', id]                           | 60 s                                        |
 * | ['message', id, 'events']                | 30 s; invalidated on `outbound.status` for id |
 * | ['draft', id]                            | Infinity (owned by its compose window)      |
 * | ['contacts', q]                          | 5 min                                       |
 * | ['admin', …]  → `adminKeys` in ./admin.ts [F] | 0                                      |
 *
 * Use the `*All()` prefixes for invalidation, e.g. `invalidateQueries({ queryKey: queryKeys.threadsAll() })`.
 * Thread lists: fetch with `listThreadsFor(filter, cursor, limit)` (endpoints.ts) using the same
 * filter as the key, so the request always matches the cache entry.
 *
 * `['draft', id]` is never stale and never invalidated, so its owner (WP-F) keeps it correct:
 * - after undo-send and cancel-schedule, seed it with the returned Draft
 *   (`queryClient.setQueryData(queryKeys.draft(d.id), d)`) before reopening the window — otherwise
 *   an older cached version makes the first autosave fail with 409 `version_conflict`;
 * - after every successful save, `setQueryData` the returned Draft;
 * - when a window closes, `queryClient.removeQueries({ queryKey: queryKeys.draft(id) })`.
 */
import type { FolderId } from './types';

/** Selects one thread list view. Exactly one of folder / labelId / q is non-null. */
export interface ThreadListFilter {
  folder: FolderId | null;
  labelId: number | null;
  q: string | null;
}

/** Normalizes a (partial) filter so equivalent views always produce the same key. */
export function threadListFilter(f: Partial<ThreadListFilter>): ThreadListFilter {
  const q = f.q?.trim() ? f.q.trim() : null;
  if (q !== null) return { folder: null, labelId: null, q };
  if (f.labelId !== undefined && f.labelId !== null) return { folder: null, labelId: f.labelId, q: null };
  return { folder: f.folder ?? 'inbox', labelId: null, q: null };
}

export const queryKeys = {
  me: () => ['me'] as const,
  labels: () => ['labels'] as const,
  counts: () => ['counts'] as const,

  /** Prefix of every thread list page. */
  threadsAll: () => ['threads'] as const,
  /** One page of a thread list; `cursor` null = first page. */
  threads: (filter: Partial<ThreadListFilter>, cursor: string | null = null) =>
    ['threads', threadListFilter(filter), cursor] as const,

  /** Prefix of every thread detail. */
  threadAll: () => ['thread'] as const,
  thread: (threadId: number) => ['thread', threadId] as const,

  /** GET /api/messages/:id/events (DeliveryStatus). WP-E invalidates it on `outbound.status` for that message. */
  messageEvents: (messageId: number) => ['message', messageId, 'events'] as const,

  draftAll: () => ['draft'] as const,
  draft: (draftId: number) => ['draft', draftId] as const,

  contactsAll: () => ['contacts'] as const,
  contacts: (q: string) => ['contacts', q.trim().toLowerCase()] as const,
  // Admin keys: `adminKeys` in ./admin.ts (owned by WP-F), all under the ['admin'] prefix.
} as const;

const MIN = 60_000;

/** staleTime per key family (ms). */
export const staleTimes = {
  me: 5 * MIN,
  labels: 5 * MIN,
  counts: 15_000,
  threads: 30_000,
  thread: 60_000,
  messageEvents: 30_000,
  draft: Number.POSITIVE_INFINITY,
  contacts: 5 * MIN,
  admin: 0,
} as const;

/** `['counts']` refetchInterval while the WebSocket is not open. */
export const COUNTS_POLL_INTERVAL_MS = 60_000;
