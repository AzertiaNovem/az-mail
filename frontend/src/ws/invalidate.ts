/**
 * WebSocket event → TanStack Query cache [WP-E] (DESIGN.md §5 "Query keys and invalidation").
 *
 * Events are invalidation hints, not data. Invalidations are coalesced: the first event opens
 * a 300 ms window, every event in it is merged, and one batch of `invalidateQueries` runs when
 * it closes (a burst of 20 `threads.changed` refetches each list once). `outbound.status` also
 * patches the cached `['thread', id]` in place right away (status chip updates without a
 * refetch).
 *
 * | Event              | Effect                                                              |
 * |--------------------|---------------------------------------------------------------------|
 * | `ready`            | threads (all lists), thread (all details), counts                  |
 * | `mail.new`         | threads, counts, thread(thread_id)                                  |
 * | `threads.changed`  | threads, counts, thread(id) for each id                             |
 * | `outbound.status`  | patch thread(thread_id) in place (else invalidate it); threads,     |
 * |                    | counts, message events(message_id)                                  |
 * | `labels.changed`   | labels, counts                                                      |
 * | `settings.changed` | me, threads (page size may have changed)                            |
 * | `pong`, `session.revoked` | nothing here (the socket handles them)                       |
 *
 * Only active queries refetch; inactive ones are marked stale. `['draft', id]` is never touched.
 */
import type { QueryClient } from '@tanstack/react-query';
import { queryKeys } from '@/api/queryKeys';
import type { ThreadDetail, WsOutboundStatusEvent, WsServerEvent } from '@/api/types';

export const COALESCE_MS = 300;

export interface EventEffects {
  threads?: boolean;
  counts?: boolean;
  labels?: boolean;
  me?: boolean;
  /** Every `['thread', …]` detail. */
  allThreads?: boolean;
  threadIds?: number[];
  eventMessageIds?: number[];
}

/** What an event invalidates (pure; the in-place outbound patch is separate). */
export function eventEffects(e: WsServerEvent): EventEffects {
  switch (e.type) {
    case 'ready':
      return { threads: true, counts: true, allThreads: true };
    case 'mail.new':
      return { threads: true, counts: true, threadIds: [e.thread_id] };
    case 'threads.changed':
      return { threads: true, counts: true, threadIds: Array.isArray(e.thread_ids) ? e.thread_ids : [] };
    case 'outbound.status':
      return { threads: true, counts: true, eventMessageIds: [e.message_id] };
    case 'labels.changed':
      return { labels: true, counts: true };
    case 'settings.changed':
      return { me: true, threads: true };
    default:
      return {};
  }
}

/**
 * Applies an `outbound.status` event to a cached thread. Returns the patched detail, or null
 * when the message (or its outbound) is not in it.
 */
export function patchOutboundStatus(detail: ThreadDetail, e: WsOutboundStatusEvent): ThreadDetail | null {
  let found = false;
  const messages = detail.messages.map((m) => {
    if (m.id !== e.message_id || !m.outbound) return m;
    found = true;
    return { ...m, outbound: { ...m.outbound, status: e.status, status_detail: e.status_detail } };
  });
  return found ? { ...detail, messages } : null;
}

interface Pending {
  threads: boolean;
  counts: boolean;
  labels: boolean;
  me: boolean;
  allThreads: boolean;
  threadIds: Set<number>;
  eventMessageIds: Set<number>;
}

const emptyPending = (): Pending => ({
  threads: false,
  counts: false,
  labels: false,
  me: false,
  allThreads: false,
  threadIds: new Set(),
  eventMessageIds: new Set(),
});

export interface Invalidator {
  handle(e: WsServerEvent): void;
  /** Runs the pending batch now. */
  flush(): void;
  /** Cancels the pending batch (on teardown). */
  dispose(): void;
}

export interface InvalidatorOptions {
  delayMs?: number;
}

export function createInvalidator(qc: QueryClient, opts: InvalidatorOptions = {}): Invalidator {
  const delay = opts.delayMs ?? COALESCE_MS;
  let pending = emptyPending();
  let timer: ReturnType<typeof setTimeout> | null = null;

  const flush = () => {
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
    const p = pending;
    pending = emptyPending();
    if (p.threads) void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
    if (p.allThreads) void qc.invalidateQueries({ queryKey: queryKeys.threadAll() });
    else for (const id of p.threadIds) void qc.invalidateQueries({ queryKey: queryKeys.thread(id), exact: true });
    if (p.counts) void qc.invalidateQueries({ queryKey: queryKeys.counts() });
    if (p.labels) void qc.invalidateQueries({ queryKey: queryKeys.labels() });
    if (p.me) void qc.invalidateQueries({ queryKey: queryKeys.me() });
    for (const id of p.eventMessageIds) void qc.invalidateQueries({ queryKey: queryKeys.messageEvents(id) });
  };

  const merge = (fx: EventEffects) => {
    const before =
      pending.threads ||
      pending.counts ||
      pending.labels ||
      pending.me ||
      pending.allThreads ||
      pending.threadIds.size > 0 ||
      pending.eventMessageIds.size > 0;
    pending.threads ||= !!fx.threads;
    pending.counts ||= !!fx.counts;
    pending.labels ||= !!fx.labels;
    pending.me ||= !!fx.me;
    pending.allThreads ||= !!fx.allThreads;
    for (const id of fx.threadIds ?? []) if (Number.isFinite(id)) pending.threadIds.add(id);
    for (const id of fx.eventMessageIds ?? []) if (Number.isFinite(id)) pending.eventMessageIds.add(id);
    const after =
      pending.threads ||
      pending.counts ||
      pending.labels ||
      pending.me ||
      pending.allThreads ||
      pending.threadIds.size > 0 ||
      pending.eventMessageIds.size > 0;
    if (!before && after && timer === null) timer = setTimeout(flush, delay);
  };

  return {
    handle(e) {
      if (!e || typeof e !== 'object' || typeof e.type !== 'string') return;
      const fx = eventEffects(e);
      if (e.type === 'outbound.status') {
        const key = queryKeys.thread(e.thread_id);
        const cached = qc.getQueryData<ThreadDetail>(key);
        const patched = cached ? patchOutboundStatus(cached, e) : null;
        if (patched) qc.setQueryData(key, patched);
        else fx.threadIds = [e.thread_id];
      }
      merge(fx);
    },
    flush,
    dispose() {
      if (timer !== null) clearTimeout(timer);
      timer = null;
      pending = emptyPending();
    },
  };
}
