import { QueryClient } from '@tanstack/react-query';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { queryKeys } from '@/api/queryKeys';
import type { Counts, ThreadDetail, ThreadListResponse } from '@/api/types';
import { setToken, clearToken } from '@/stores/auth';
import { useToastStore } from '@/stores/toast';
import { detail, item, message, page } from './testFixtures';
import {
  applyToItem,
  inverseAction,
  moveThreads,
  patchMessageOptimistic,
  performThreadAction,
  removesFromView,
  stateFromDetail,
  transformCounts,
  transformListPage,
  transformThreadDetail,
} from './threadActions';

const inbox = { folder: 'inbox' as const, labelId: null, q: null };
const all = { folder: 'all' as const, labelId: null, q: null };
const starred = { folder: 'starred' as const, labelId: null, q: null };
const trash = { folder: 'trash' as const, labelId: null, q: null };
const label3 = { folder: null, labelId: 3, q: null };
const search = { folder: null, labelId: null, q: 'x' };

describe('pure transforms', () => {
  it('knows which views a thread leaves', () => {
    expect(removesFromView(inbox, { ids: [1], action: 'archive' })).toBe(true);
    expect(removesFromView(all, { ids: [1], action: 'archive' })).toBe(false);
    expect(removesFromView(search, { ids: [1], action: 'trash' })).toBe(true);
    expect(removesFromView(trash, { ids: [1], action: 'trash' })).toBe(false);
    expect(removesFromView(trash, { ids: [1], action: 'restore' })).toBe(true);
    expect(removesFromView(starred, { ids: [1], action: 'unstar' })).toBe(true);
    expect(removesFromView(label3, { ids: [1], action: 'remove_label', labelId: 3 })).toBe(true);
    expect(removesFromView(label3, { ids: [1], action: 'remove_label', labelId: 4 })).toBe(false);
    expect(removesFromView(all, { ids: [1], action: 'delete_forever' })).toBe(true);
    expect(removesFromView(inbox, { ids: [1], action: 'read' })).toBe(false);
  });

  it('updates rows', () => {
    const row = item(1, { unread: true, participants: [{ name: 'a', email: 'a@x', is_me: false, unread: true }], label_ids: [2] });
    expect(applyToItem(row, { ids: [1], action: 'read' })).toMatchObject({ unread: false, participants: [{ unread: false }] });
    expect(applyToItem(row, { ids: [1], action: 'star' }).starred).toBe(true);
    expect(applyToItem(row, { ids: [1], action: 'archive' }).in_inbox).toBe(false);
    expect(applyToItem(row, { ids: [1], action: 'add_label', labelId: 3 }).label_ids).toEqual([2, 3]);
    expect(applyToItem(row, { ids: [1], action: 'add_label', labelId: 2 })).toBe(row);
    expect(applyToItem(row, { ids: [1], action: 'remove_label', labelId: 2 }).label_ids).toEqual([]);
  });

  it('transforms a list page and its total', () => {
    const p = page([item(1), item(2), item(3)], { total: 10 });
    const archived = transformListPage(p, inbox, { ids: [1, 3], action: 'archive' });
    expect(archived.items.map((i) => i.id)).toEqual([2]);
    expect(archived.total).toBe(8);
    expect(transformListPage(p, all, { ids: [1], action: 'archive' }).items[0]!.in_inbox).toBe(false);
    expect(transformListPage(p, inbox, { ids: [99], action: 'archive' })).toBe(p);
    expect(transformListPage(page([item(1)], { total: null }), inbox, { ids: [1], action: 'trash' }).total).toBeNull();
  });

  it('transforms a thread detail', () => {
    const d = detail(1, [message(1, { id: 10, is_read: false }), message(1, { id: 11, is_read: false }), message(1, { id: 12, is_draft: true })]);
    const read = transformThreadDetail(d, { ids: [1], action: 'read' });
    expect(read.messages.map((m) => m.is_read)).toEqual([true, true, true]);
    const unread = transformThreadDetail(read, { ids: [1], action: 'unread' });
    expect(unread.messages.map((m) => m.is_read)).toEqual([true, false, true]);
    expect(transformThreadDetail(d, { ids: [1], action: 'star' }).messages.map((m) => m.is_starred)).toEqual([false, true, false]);
    expect(transformThreadDetail(d, { ids: [1], action: 'trash' }).messages.every((m) => m.trashed)).toBe(true);
    const labelled = transformThreadDetail(d, { ids: [1], action: 'add_label', labelId: 5 });
    expect(labelled.label_ids).toEqual([5]);
    expect(labelled.messages[0]!.label_ids).toEqual([5]);
    expect(transformThreadDetail(labelled, { ids: [1], action: 'remove_label', labelId: 5 }).label_ids).toEqual([]);
  });

  it('predicts inbox / label unread counts', () => {
    const counts: Counts = { inbox_unread: 5, drafts: 0, scheduled: 0, spam_unread: 0, labels: { '3': { unread: 2, total: 4 } } };
    const unreadInbox = { unread: true, inInbox: true, labelIds: [3] };
    const readInbox = { unread: false, inInbox: true, labelIds: [] };
    expect(transformCounts(counts, [unreadInbox, readInbox], { ids: [1, 2], action: 'read' })).toMatchObject({
      inbox_unread: 4,
      labels: { '3': { unread: 1, total: 4 } },
    });
    expect(transformCounts(counts, [readInbox], { ids: [2], action: 'unread' }).inbox_unread).toBe(6);
    expect(transformCounts(counts, [unreadInbox], { ids: [1], action: 'archive' }).inbox_unread).toBe(4);
    expect(transformCounts(counts, [unreadInbox], { ids: [1], action: 'spam' })).toMatchObject({ inbox_unread: 4, spam_unread: 1 });
    expect(transformCounts({ ...counts, inbox_unread: 0 }, [unreadInbox], { ids: [1], action: 'read' }).inbox_unread).toBe(0);
    expect(stateFromDetail(detail(1, [message(1, { is_read: false, in_inbox: false }), message(1, { is_draft: true, is_read: false })]))).toEqual({
      unread: true,
      inInbox: false,
      labelIds: [],
    });
  });

  it('knows inverse actions', () => {
    expect(inverseAction('archive')).toBe('inbox');
    expect(inverseAction('trash')).toBe('restore');
    expect(inverseAction('spam')).toBe('not_spam');
    expect(inverseAction('add_label')).toBe('remove_label');
    expect(inverseAction('delete_forever')).toBeNull();
  });
});

// ───────────── optimistic flow against a mocked API ─────────────

type FetchCall = { url: string; init: RequestInit };

function mockFetch(handler: (url: string, init: RequestInit) => Response | Promise<Response>) {
  const calls: FetchCall[] = [];
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string, init: RequestInit) => {
      calls.push({ url, init });
      return handler(url, init);
    }),
  );
  return calls;
}
const json = (status: number, body: unknown) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });

describe('performThreadAction', () => {
  let qc: QueryClient;
  beforeEach(() => {
    setToken('t');
    qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
    qc.setQueryData<ThreadListResponse>(queryKeys.threads(inbox), page([item(1, { unread: true }), item(2)], { total: 2 }));
    qc.setQueryData<ThreadListResponse>(queryKeys.threads(all), page([item(1, { unread: true }), item(2)], { total: 2 }));
    qc.setQueryData<ThreadDetail>(queryKeys.thread(1), detail(1, [message(1, { id: 10, is_read: false })]));
    qc.setQueryData<Counts>(queryKeys.counts(), { inbox_unread: 1, drafts: 0, scheduled: 0, spam_unread: 0, labels: {} });
  });
  afterEach(() => {
    clearToken();
    useToastStore.getState().clear();
    qc.clear();
  });

  it('applies optimistically, calls the API and shows an undo toast that runs the inverse', async () => {
    let resolve!: (r: Response) => void;
    const calls = mockFetch((url, init) => {
      if (url.endsWith('/api/threads/actions') && JSON.parse(String(init.body)).action === 'archive')
        return new Promise<Response>((r) => (resolve = r));
      return json(200, { thread_ids: [1] });
    });
    const done = performThreadAction(qc, { ids: [1], action: 'archive' }, { toast: true });
    await vi.waitFor(() => expect(calls).toHaveLength(1));
    // Optimistic state while the request is in flight.
    expect(qc.getQueryData<ThreadListResponse>(queryKeys.threads(inbox))!.items.map((i) => i.id)).toEqual([2]);
    expect(qc.getQueryData<ThreadListResponse>(queryKeys.threads(all))!.items[0]!.in_inbox).toBe(false);
    expect(qc.getQueryData<ThreadDetail>(queryKeys.thread(1))!.messages[0]!.in_inbox).toBe(false);
    expect(qc.getQueryData<Counts>(queryKeys.counts())!.inbox_unread).toBe(0);
    resolve(json(200, { thread_ids: [1] }));
    expect(await done).toBe(true);
    expect(JSON.parse(String(calls[0]!.init.body))).toEqual({ thread_ids: [1], action: 'archive' });
    expect((calls[0]!.init.headers as Record<string, string>).Authorization).toBe('Bearer t');

    const t = useToastStore.getState().toasts.at(-1)!;
    expect(t.message).toBe('已归档 1 个会话');
    expect(t.actions.map((a) => a.label)).toEqual(['撤销']);
    t.actions[0]!.onClick();
    await vi.waitFor(() => expect(calls).toHaveLength(2));
    expect(JSON.parse(String(calls[1]!.init.body))).toEqual({ thread_ids: [1], action: 'inbox' });
    await vi.waitFor(() => expect(useToastStore.getState().toasts.at(-1)!.message).toBe('已撤消操作'));
  });

  it('rolls every cache back and shows an error toast when the request fails', async () => {
    const before = {
      inbox: qc.getQueryData(queryKeys.threads(inbox)),
      all: qc.getQueryData(queryKeys.threads(all)),
      thread: qc.getQueryData(queryKeys.thread(1)),
      counts: qc.getQueryData(queryKeys.counts()),
    };
    mockFetch(() => json(500, { error: { code: 'internal_error', message: '服务器内部错误', details: {} } }));
    expect(await performThreadAction(qc, { ids: [1], action: 'trash' }, { toast: true })).toBe(false);
    expect(qc.getQueryData(queryKeys.threads(inbox))).toEqual(before.inbox);
    expect(qc.getQueryData(queryKeys.threads(all))).toEqual(before.all);
    expect(qc.getQueryData(queryKeys.thread(1))).toEqual(before.thread);
    expect(qc.getQueryData(queryKeys.counts())).toEqual(before.counts);
    const t = useToastStore.getState().toasts.at(-1)!;
    expect(t.tone).toBe('error');
    expect(t.message).toBe('操作失败：服务器内部错误');
  });

  it('sends label_id for label actions and removes the detail after delete_forever', async () => {
    const calls = mockFetch(() => json(200, { thread_ids: [1] }));
    await performThreadAction(qc, { ids: [1], action: 'add_label', labelId: 7 }, { toast: true, labelName: '工作' });
    expect(JSON.parse(String(calls[0]!.init.body))).toEqual({ thread_ids: [1], action: 'add_label', label_id: 7 });
    expect(useToastStore.getState().toasts.at(-1)!.message).toBe('已为 1 个会话添加标签“工作”');
    await performThreadAction(qc, { ids: [1], action: 'delete_forever' });
    expect(qc.getQueryData(queryKeys.thread(1))).toBeUndefined();
    expect(await performThreadAction(qc, { ids: [], action: 'archive' })).toBe(true);
    expect(calls).toHaveLength(2);
  });

  it('moveThreads to a label adds it and archives from the inbox', async () => {
    const calls = mockFetch(() => json(200, { thread_ids: [2] }));
    expect(await moveThreads(qc, [2], { kind: 'label', labelId: 4, name: '旅行' }, { folder: 'inbox', labelId: null })).toBe(true);
    expect(calls.map((c) => JSON.parse(String(c.init.body)).action)).toEqual(['add_label', 'archive']);
    expect(useToastStore.getState().toasts.at(-1)!.message).toBe('已将 1 个会话移至“旅行”');
    calls.length = 0;
    await moveThreads(qc, [2], { kind: 'inbox' }, { folder: 'trash', labelId: null });
    expect(calls.map((c) => JSON.parse(String(c.init.body)).action)).toEqual(['restore', 'inbox']);
    calls.length = 0;
    await moveThreads(qc, [2], { kind: 'label', labelId: 4, name: '旅行' }, { folder: null, labelId: 3 });
    expect(calls.map((c) => JSON.parse(String(c.init.body)))).toEqual([
      { thread_ids: [2], action: 'add_label', label_id: 4 },
      { thread_ids: [2], action: 'remove_label', label_id: 3 },
    ]);
  });

  it('patchMessageOptimistic stars one message and rolls back on failure', async () => {
    mockFetch(() => json(200, { ...message(1, { id: 10, is_starred: true }), snippet: 'server' }));
    expect(await patchMessageOptimistic(qc, 1, 10, { is_starred: true })).toBe(true);
    expect(qc.getQueryData<ThreadDetail>(queryKeys.thread(1))!.messages[0]).toMatchObject({ is_starred: true, snippet: 'server' });

    mockFetch(() => json(404, { error: { code: 'not_found', message: '内容不存在或已被删除', details: {} } }));
    const before = qc.getQueryData(queryKeys.thread(1));
    expect(await patchMessageOptimistic(qc, 1, 10, { is_read: true })).toBe(false);
    expect(qc.getQueryData(queryKeys.thread(1))).toEqual(before);
  });
});
