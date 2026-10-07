/**
 * Optimistic thread actions [WP-E] (DESIGN.md §5): `onMutate` cancels in-flight thread
 * queries, snapshots every `['threads', …]` page, the affected `['thread', id]` details and
 * `['counts']`, and transforms them (remove from the folder on archive / trash / spam, flip
 * unread / star, add / remove labels); on error everything is rolled back and an error toast
 * shown; when settled the lists, details and counts are invalidated so the server wins.
 * Archive, trash and spam (and their inverses) show a "撤销" toast running the inverse action.
 */
import type { QueryClient, QueryKey } from '@tanstack/react-query';
import { patchMessage, threadActions } from '@/api/endpoints';
import { errorMessage } from '@/api/client';
import { queryKeys, type ThreadListFilter } from '@/api/queryKeys';
import type {
  Counts,
  Message,
  MessagePatch,
  ThreadAction,
  ThreadActionRequest,
  ThreadDetail,
  ThreadListItem,
  ThreadListResponse,
} from '@/api/types';
import { t, type MessageKey } from '@/i18n/zh';
import { toast } from '@/stores/toast';

export interface ActionRequest {
  ids: number[];
  action: ThreadAction;
  /** Required for add_label / remove_label. */
  labelId?: number;
}

// ───────────── pure transforms ─────────────

/** Whether the thread leaves the list `filter` after `action`. */
export function removesFromView(filter: ThreadListFilter, req: ActionRequest): boolean {
  const folder = filter.folder;
  switch (req.action) {
    case 'archive':
      return folder === 'inbox';
    case 'trash':
      return folder !== 'trash';
    case 'spam':
      return folder !== 'spam';
    case 'delete_forever':
      return true;
    case 'restore':
      return folder === 'trash';
    case 'not_spam':
      return folder === 'spam';
    case 'unstar':
      return folder === 'starred';
    case 'remove_label':
      return filter.labelId !== null && filter.labelId === req.labelId;
    default:
      return false;
  }
}

/** The list row after `action` (when it stays in the view). */
export function applyToItem(item: ThreadListItem, req: ActionRequest): ThreadListItem {
  switch (req.action) {
    case 'archive':
      return { ...item, in_inbox: false };
    case 'inbox':
    case 'not_spam':
      return { ...item, in_inbox: true };
    case 'read':
      return { ...item, unread: false, participants: item.participants.map((p) => ({ ...p, unread: false })) };
    case 'unread':
      return { ...item, unread: true };
    case 'star':
      return { ...item, starred: true };
    case 'unstar':
      return { ...item, starred: false };
    case 'add_label':
      return req.labelId === undefined || item.label_ids.includes(req.labelId)
        ? item
        : { ...item, label_ids: [...item.label_ids, req.labelId] };
    case 'remove_label':
      return { ...item, label_ids: item.label_ids.filter((l) => l !== req.labelId) };
    default:
      return item;
  }
}

/** One cached list page after `action`. */
export function transformListPage(page: ThreadListResponse, filter: ThreadListFilter, req: ActionRequest): ThreadListResponse {
  const ids = new Set(req.ids);
  if (!page.items.some((i) => ids.has(i.id))) return page;
  const remove = removesFromView(filter, req);
  let removed = 0;
  const items: ThreadListItem[] = [];
  for (const item of page.items) {
    if (!ids.has(item.id)) items.push(item);
    else if (remove) removed++;
    else items.push(applyToItem(item, req));
  }
  return { ...page, items, total: page.total === null ? null : Math.max(0, page.total - removed) };
}

const visibleNonDraft = (m: Message) => !m.is_draft;

/** A cached thread detail after `action`. */
export function transformThreadDetail(detail: ThreadDetail, req: ActionRequest): ThreadDetail {
  const map = (f: (m: Message) => Message) => ({ ...detail, messages: detail.messages.map(f) });
  const lastId = [...detail.messages].reverse().find(visibleNonDraft)?.id;
  switch (req.action) {
    case 'archive':
      return map((m) => ({ ...m, in_inbox: false }));
    case 'inbox':
      return map((m) => (m.is_draft ? m : { ...m, in_inbox: true }));
    case 'read':
      return map((m) => (m.is_draft ? m : { ...m, is_read: true }));
    case 'unread':
      return map((m) => (m.id === lastId ? { ...m, is_read: false } : m));
    case 'star':
      return map((m) => (m.id === lastId ? { ...m, is_starred: true } : m));
    case 'unstar':
      return map((m) => ({ ...m, is_starred: false }));
    case 'trash':
      return map((m) => ({ ...m, trashed: true }));
    case 'restore':
      return map((m) => ({ ...m, trashed: false }));
    case 'spam':
      return map((m) => ({ ...m, is_spam: true }));
    case 'not_spam':
      return map((m) => ({ ...m, is_spam: false, in_inbox: m.is_draft ? m.in_inbox : true }));
    case 'add_label': {
      const id = req.labelId;
      if (id === undefined) return detail;
      const add = (l: number[]) => (l.includes(id) ? l : [...l, id]);
      return { ...detail, label_ids: add(detail.label_ids), messages: detail.messages.map((m) => ({ ...m, label_ids: add(m.label_ids) })) };
    }
    case 'remove_label': {
      const drop = (l: number[]) => l.filter((x) => x !== req.labelId);
      return { ...detail, label_ids: drop(detail.label_ids), messages: detail.messages.map((m) => ({ ...m, label_ids: drop(m.label_ids) })) };
    }
    default:
      return detail;
  }
}

/** What counts need to know about a thread before the action. */
export interface ThreadState {
  unread: boolean;
  inInbox: boolean;
  labelIds: number[];
}

export function stateFromItem(i: ThreadListItem): ThreadState {
  return { unread: i.unread, inInbox: i.in_inbox, labelIds: i.label_ids };
}

export function stateFromDetail(d: ThreadDetail): ThreadState {
  const live = d.messages.filter((m) => !m.is_draft && !m.trashed && !m.is_spam);
  return { unread: live.some((m) => !m.is_read), inInbox: live.some((m) => m.in_inbox), labelIds: d.label_ids };
}

/** `['counts']` after the action, for the counters that can be predicted (inbox / label unread). */
export function transformCounts(counts: Counts, states: ThreadState[], req: ActionRequest): Counts {
  let inbox = counts.inbox_unread;
  let spam = counts.spam_unread;
  const labels: Counts['labels'] = { ...counts.labels };
  const bumpLabels = (s: ThreadState, delta: number) => {
    for (const id of s.labelIds) {
      const c = labels[String(id)];
      if (c) labels[String(id)] = { ...c, unread: Math.max(0, c.unread + delta) };
    }
  };
  for (const s of states) {
    switch (req.action) {
      case 'read':
        if (s.unread) {
          if (s.inInbox) inbox--;
          bumpLabels(s, -1);
        }
        break;
      case 'unread':
        if (!s.unread) {
          if (s.inInbox) inbox++;
          bumpLabels(s, +1);
        }
        break;
      case 'archive':
        if (s.unread && s.inInbox) inbox--;
        break;
      case 'trash':
      case 'delete_forever':
        if (s.unread && s.inInbox) inbox--;
        if (s.unread) bumpLabels(s, -1);
        break;
      case 'spam':
        if (s.unread && s.inInbox) inbox--;
        if (s.unread) {
          spam++;
          bumpLabels(s, -1);
        }
        break;
      default:
        break;
    }
  }
  return { ...counts, inbox_unread: Math.max(0, inbox), spam_unread: Math.max(0, spam), labels };
}

/** The action that undoes `action` (null when it cannot be undone). */
export function inverseAction(action: ThreadAction): ThreadAction | null {
  const inv: Partial<Record<ThreadAction, ThreadAction>> = {
    archive: 'inbox',
    inbox: 'archive',
    trash: 'restore',
    restore: 'trash',
    spam: 'not_spam',
    not_spam: 'spam',
    read: 'unread',
    unread: 'read',
    star: 'unstar',
    unstar: 'star',
    add_label: 'remove_label',
    remove_label: 'add_label',
  };
  return inv[action] ?? null;
}

/** Actions that move threads between folders (these get an undo toast). */
export const UNDOABLE_MOVES: ReadonlySet<ThreadAction> = new Set(['archive', 'trash', 'spam', 'not_spam', 'restore', 'inbox']);

const TOAST_KEYS: Partial<Record<ThreadAction, MessageKey>> = {
  archive: 'mail.toasts.archived',
  trash: 'mail.toasts.trashed',
  spam: 'mail.toasts.spammed',
  not_spam: 'mail.toasts.notSpam',
  restore: 'mail.toasts.restored',
  inbox: 'mail.toasts.movedToInbox',
  delete_forever: 'mail.toasts.deletedForever',
  read: 'mail.toasts.markedRead',
  unread: 'mail.toasts.markedUnread',
  add_label: 'mail.toasts.labelAdded',
  remove_label: 'mail.toasts.labelRemoved',
};

export function actionToastMessage(req: ActionRequest, labelName = ''): string | null {
  const key = TOAST_KEYS[req.action];
  return key ? t(key, { count: req.ids.length, name: labelName }) : null;
}

// ───────────── cache plumbing ─────────────

export interface Snapshot {
  lists: [QueryKey, ThreadListResponse | undefined][];
  details: [QueryKey, ThreadDetail | undefined][];
  counts: Counts | undefined;
}

function isFilter(v: unknown): v is ThreadListFilter {
  return typeof v === 'object' && v !== null && 'folder' in v && 'labelId' in v && 'q' in v;
}

/** Collects the pre-action state of the affected threads from any cache entry. */
function collectStates(qc: QueryClient, ids: number[]): ThreadState[] {
  const found = new Map<number, ThreadState>();
  for (const id of ids) {
    const d = qc.getQueryData<ThreadDetail>(queryKeys.thread(id));
    if (d) found.set(id, stateFromDetail(d));
  }
  for (const [, page] of qc.getQueriesData<ThreadListResponse>({ queryKey: queryKeys.threadsAll() })) {
    for (const item of page?.items ?? []) if (ids.includes(item.id) && !found.has(item.id)) found.set(item.id, stateFromItem(item));
  }
  return [...found.values()];
}

/** Applies the optimistic update and returns what is needed to roll it back. */
export async function applyOptimistic(qc: QueryClient, req: ActionRequest): Promise<Snapshot> {
  await Promise.all([
    qc.cancelQueries({ queryKey: queryKeys.threadsAll() }),
    ...req.ids.map((id) => qc.cancelQueries({ queryKey: queryKeys.thread(id), exact: true })),
    qc.cancelQueries({ queryKey: queryKeys.counts() }),
  ]);
  const states = collectStates(qc, req.ids);
  const snapshot: Snapshot = {
    lists: qc.getQueriesData<ThreadListResponse>({ queryKey: queryKeys.threadsAll() }),
    details: req.ids.map((id) => [queryKeys.thread(id), qc.getQueryData<ThreadDetail>(queryKeys.thread(id))]),
    counts: qc.getQueryData<Counts>(queryKeys.counts()),
  };
  for (const [key, page] of snapshot.lists) {
    const filter = key[1];
    if (!page || !isFilter(filter)) continue;
    const next = transformListPage(page, filter, req);
    if (next !== page) qc.setQueryData(key, next);
  }
  for (const [key, detail] of snapshot.details) if (detail) qc.setQueryData(key, transformThreadDetail(detail, req));
  if (snapshot.counts) qc.setQueryData(queryKeys.counts(), transformCounts(snapshot.counts, states, req));
  return snapshot;
}

export function rollback(qc: QueryClient, snap: Snapshot): void {
  for (const [key, page] of snap.lists) qc.setQueryData(key, page);
  for (const [key, detail] of snap.details) qc.setQueryData(key, detail);
  if (snap.counts) qc.setQueryData(queryKeys.counts(), snap.counts);
}

function invalidateAfter(qc: QueryClient, ids: number[], removedDetails: boolean): void {
  void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
  void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  for (const id of ids) {
    if (removedDetails) qc.removeQueries({ queryKey: queryKeys.thread(id), exact: true });
    else void qc.invalidateQueries({ queryKey: queryKeys.thread(id), exact: true });
  }
}

export interface PerformOptions {
  /** Show the success toast (with 撤销 for folder moves). Default false. */
  toast?: boolean;
  /** Label name for label toasts. */
  labelName?: string;
  /** Whether the success toast offers 撤销 (default: for folder moves and label changes). */
  undo?: boolean;
  /** Called after a successful undo (e.g. to navigate back to the thread). */
  onUndone?: () => void;
}

function toRequest(req: ActionRequest): ThreadActionRequest {
  if (req.action === 'add_label' || req.action === 'remove_label') {
    if (req.labelId === undefined) throw new Error(`${req.action} needs labelId`);
    return { thread_ids: req.ids, action: req.action, label_id: req.labelId };
  }
  return { thread_ids: req.ids, action: req.action };
}

/**
 * Runs a thread action with an optimistic cache update. Resolves true on success; on failure
 * the caches are restored, an error toast is shown and it resolves false (never rejects).
 */
export async function performThreadAction(qc: QueryClient, req: ActionRequest, opts: PerformOptions = {}): Promise<boolean> {
  if (req.ids.length === 0) return true;
  const snapshot = await applyOptimistic(qc, req);
  try {
    await threadActions(toRequest(req));
  } catch (e) {
    rollback(qc, snapshot);
    toast.error(t('mail.toasts.failed', { message: errorMessage(e) }));
    invalidateAfter(qc, req.ids, false);
    return false;
  }
  invalidateAfter(qc, req.ids, req.action === 'delete_forever');

  if (opts.toast) {
    const message = actionToastMessage(req, opts.labelName);
    const inverse = inverseAction(req.action);
    const undoable =
      (opts.undo ?? (UNDOABLE_MOVES.has(req.action) || req.action === 'add_label' || req.action === 'remove_label')) &&
      inverse !== null;
    if (message) {
      toast.push({
        message,
        actions: undoable
          ? [
              {
                label: t('mail.actions.undo'),
                onClick: () => {
                  void performThreadAction(qc, { ...req, action: inverse }).then((ok) => {
                    if (!ok) return;
                    toast.push({ message: t('mail.toasts.undone') });
                    opts.onUndone?.();
                  });
                },
              },
            ]
          : [],
      });
    }
  }
  return true;
}

/** Where "移至" sends threads. */
export type MoveDestination = { kind: 'inbox' } | { kind: 'spam' } | { kind: 'trash' } | { kind: 'label'; labelId: number; name: string };

export interface MoveContext {
  /** Current folder (null in label / search views). */
  folder: string | null;
  /** Current label view, if any. */
  labelId: number | null;
}

/**
 * Gmail "移至": inbox / spam / trash are one action (with 撤销); a label is "add it, then leave
 * the current place" (archive from the inbox, remove the current label in a label view).
 */
export async function moveThreads(qc: QueryClient, ids: number[], dest: MoveDestination, ctx: MoveContext): Promise<boolean> {
  if (ids.length === 0) return true;
  switch (dest.kind) {
    case 'spam':
      return performThreadAction(qc, { ids, action: 'spam' }, { toast: true });
    case 'trash':
      return performThreadAction(qc, { ids, action: 'trash' }, { toast: true });
    case 'inbox':
      if (ctx.folder === 'spam') return performThreadAction(qc, { ids, action: 'not_spam' }, { toast: true });
      if (ctx.folder === 'trash') {
        if (!(await performThreadAction(qc, { ids, action: 'restore' }))) return false;
      }
      return performThreadAction(qc, { ids, action: 'inbox' }, { toast: true });
    case 'label': {
      if (!(await performThreadAction(qc, { ids, action: 'add_label', labelId: dest.labelId }))) return false;
      let ok = true;
      if (ctx.folder === 'inbox') ok = await performThreadAction(qc, { ids, action: 'archive' });
      else if (ctx.labelId !== null && ctx.labelId !== dest.labelId)
        ok = await performThreadAction(qc, { ids, action: 'remove_label', labelId: ctx.labelId });
      if (ok) toast.push({ message: t('mail.toasts.movedToLabel', { count: ids.length, name: dest.name }) });
      return ok;
    }
  }
}

/**
 * PATCH /api/messages/:id with an optimistic update of `['thread', threadId]` (star one message,
 * mark read / unread). The server's Message replaces the cached one; lists and counts are
 * invalidated afterwards. Resolves false (after rollback + toast) on failure.
 */
export async function patchMessageOptimistic(
  qc: QueryClient,
  threadId: number,
  messageId: number,
  patch: MessagePatch,
): Promise<boolean> {
  const key = queryKeys.thread(threadId);
  await qc.cancelQueries({ queryKey: key, exact: true });
  const before = qc.getQueryData<ThreadDetail>(key);
  const apply = (m: Message): Message => ({
    ...m,
    ...(patch.is_read !== undefined ? { is_read: patch.is_read } : {}),
    ...(patch.is_starred !== undefined ? { is_starred: patch.is_starred } : {}),
    label_ids: [
      ...m.label_ids.filter((l) => !(patch.remove_label_ids ?? []).includes(l)),
      ...(patch.add_label_ids ?? []).filter((l) => !m.label_ids.includes(l)),
    ],
  });
  if (before) qc.setQueryData<ThreadDetail>(key, { ...before, messages: before.messages.map((m) => (m.id === messageId ? apply(m) : m)) });
  try {
    const updated = await patchMessage(messageId, patch);
    const now = qc.getQueryData<ThreadDetail>(key);
    if (now && updated && typeof updated === 'object')
      qc.setQueryData<ThreadDetail>(key, { ...now, messages: now.messages.map((m) => (m.id === messageId ? { ...m, ...updated } : m)) });
  } catch (e) {
    if (before) qc.setQueryData(key, before);
    toast.error(t('mail.toasts.failed', { message: errorMessage(e) }));
    return false;
  } finally {
    void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
    void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  }
  return true;
}
