/**
 * Query hooks of the mail UI [WP-E]: one place for keys, fetchers and stale times
 * (DESIGN.md §5 "Query keys and invalidation").
 */
import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { useMemo } from 'react';
import { getCounts, getMe, getThread, listLabels, listThreadsFor } from '@/api/endpoints';
import { COUNTS_POLL_INTERVAL_MS, queryKeys, staleTimes, type ThreadListFilter } from '@/api/queryKeys';
import type { Label, Me } from '@/api/types';
import { DEFAULT_TZ } from '@/lib/format';
import { useSocketOpen } from '@/ws/socket';

export const DEFAULT_PAGE_SIZE = 50;

export function useMeQuery() {
  return useQuery({ queryKey: queryKeys.me(), queryFn: ({ signal }) => getMe(signal), staleTime: staleTimes.me });
}

/** The signed-in user. Only use below RequireAuth (which waits for `['me']`). */
export function useMe(): Me | undefined {
  return useMeQuery().data;
}

/** Lower-cased addresses that are "me": the login address and every identity. */
export function myAddressSet(me: Me | undefined): Set<string> {
  const out = new Set<string>();
  if (!me) return out;
  out.add(me.email.toLowerCase());
  for (const i of me.identities ?? []) out.add(i.email.toLowerCase());
  return out;
}

export function useMyAddresses(): Set<string> {
  const me = useMe();
  return useMemo(() => myAddressSet(me), [me]);
}

/** Display time zone (Settings.timezone, default Asia/Shanghai). */
export function useTimeZone(): string {
  return useMe()?.settings?.timezone || DEFAULT_TZ;
}

export function usePageSize(): number {
  const n = useMe()?.settings?.page_size;
  return typeof n === 'number' && n >= 10 && n <= 100 ? n : DEFAULT_PAGE_SIZE;
}

/** Labels sorted for display (sort_order, then name). */
export function sortLabels(labels: readonly Label[]): Label[] {
  return [...labels].sort((a, b) => a.sort_order - b.sort_order || a.name.localeCompare(b.name, 'zh-CN'));
}

export function useLabels() {
  return useQuery({
    queryKey: queryKeys.labels(),
    queryFn: ({ signal }) => listLabels(signal),
    staleTime: staleTimes.labels,
    select: sortLabels,
  });
}

/** `['counts']`: 15 s stale; polled every 60 s while the WebSocket is not open. */
export function useCounts() {
  const socketOpen = useSocketOpen();
  return useQuery({
    queryKey: queryKeys.counts(),
    queryFn: ({ signal }) => getCounts(signal),
    staleTime: staleTimes.counts,
    refetchInterval: socketOpen ? false : COUNTS_POLL_INTERVAL_MS,
  });
}

export function useThreadList(filter: ThreadListFilter, cursor: string | null, pageSize: number) {
  return useQuery({
    queryKey: queryKeys.threads(filter, cursor),
    queryFn: ({ signal }) => listThreadsFor(filter, cursor, pageSize, signal),
    staleTime: staleTimes.threads,
    // Keep the previous page on screen while the next one loads (pager), but not across views.
    placeholderData: (prev, prevQuery) =>
      prevQuery && JSON.stringify(prevQuery.queryKey[1]) === JSON.stringify(filter) ? keepPreviousData(prev) : undefined,
  });
}

export function useThread(threadId: number | null) {
  return useQuery({
    queryKey: queryKeys.thread(threadId ?? -1),
    queryFn: ({ signal }) => getThread(threadId as number, signal),
    staleTime: staleTimes.thread,
    enabled: threadId !== null,
  });
}
