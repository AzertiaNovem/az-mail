/**
 * Opening compose windows from the reading UI [WP-E]. The windows themselves belong to WP-F;
 * this only talks to the compose store contract (stores/compose.ts).
 */
import type { QueryClient } from '@tanstack/react-query';
import { getThread } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import type { Address, Message, ThreadDetail } from '@/api/types';
import { useComposeStore } from '@/stores/compose';

export type ReplyKind = 'reply' | 'reply_all' | 'forward';

export function openReply(kind: ReplyKind, message: Pick<Message, 'id' | 'thread_id'>): string {
  return useComposeStore.getState().open({ kind, parentMessageId: message.id, threadId: message.thread_id });
}

export function openDraft(draftId: number): string {
  return useComposeStore.getState().open({ kind: 'draft', draftId });
}

export function openNewMessage(to?: Address[], subject?: string): string {
  return useComposeStore.getState().open({
    kind: 'new',
    ...(to && to.length ? { to } : {}),
    ...(subject ? { subject } : {}),
  });
}

/** The newest draft of a thread that has drafts only (no sent/received messages), else null. */
export function draftOnlyTarget(detail: ThreadDetail): Message | null {
  const live = detail.messages.filter((m) => !m.trashed);
  if (live.length === 0 || live.some((m) => !m.is_draft)) return null;
  return [...live].sort((a, b) => b.date - a.date)[0] ?? null;
}

/** The message replies are addressed to: the newest non-draft, non-trashed message. */
export function replyTarget(messages: readonly Message[]): Message | null {
  const candidates = messages.filter((m) => !m.is_draft && !m.trashed);
  return candidates[candidates.length - 1] ?? messages.filter((m) => !m.is_draft).at(-1) ?? null;
}

/**
 * DESIGN §5: opening a thread that contains only drafts opens the compose window instead.
 * Loads the thread (cache first) and opens its newest draft; resolves false when the thread is
 * not draft-only (the caller then navigates to it normally).
 */
export async function openDraftThread(qc: QueryClient, threadId: number): Promise<boolean> {
  const detail = await qc.fetchQuery({
    queryKey: queryKeys.thread(threadId),
    queryFn: ({ signal }) => getThread(threadId, signal),
    staleTime: staleTimes.thread,
  });
  const draft = draftOnlyTarget(detail);
  if (!draft) return false;
  openDraft(draft.id);
  return true;
}
