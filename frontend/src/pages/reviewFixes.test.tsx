/**
 * Route-level regression tests for review findings in the mail views (real routes, stubbed
 * fetch): the newest message stays expanded after a reply is sent (F2), collapsed rows keep their
 * snippet on narrow screens (F5), the pager's double click (F6), mail arriving in the open thread
 * is marked read (F10), a page-size change resets the pager (F11), the mail-side label name limit
 * (F14), search thread links without ?q= (F17) and rescheduling a scheduled message (spec F5).
 */
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { queryKeys } from '@/api/queryKeys';
import type { Me, Message, ThreadDetail } from '@/api/types';
import { TooltipProvider } from '@/components/common';
import { LabelDialogHost, openLabelDialog } from '@/components/mail/LabelDialog';
import { detail, item, LABELS, ME, message, page } from '@/components/mail/testFixtures';
import { routes } from '@/router';
import { clearToken, setToken } from '@/stores/auth';
import { useComposeStore } from '@/stores/compose';
import { useToastStore } from '@/stores/toast';
import { useUiStore } from '@/stores/ui';

class QuietSocket {
  readyState = 0;
  onopen: (() => void) | null = null;
  onmessage: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: (() => void) | null = null;
  send() {}
  close() {}
}

class NoopResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

type Handler = (url: URL, init: RequestInit) => Response | Promise<Response> | undefined;
const json = (status: number, body: unknown) =>
  new Response(body === undefined ? null : JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
const calls: { url: URL; init: RequestInit }[] = [];
const actionCalls = () => calls.filter((c) => c.url.pathname === '/api/threads/actions').map((c) => JSON.parse(String(c.init.body)));

let me: Me = ME;
let thread1: ThreadDetail;

function api(extra: Handler = () => undefined) {
  vi.stubGlobal(
    'fetch',
    vi.fn(async (input: string, init: RequestInit = {}) => {
      const url = new URL(input, 'http://localhost');
      calls.push({ url, init });
      const custom = await extra(url, init);
      if (custom) return custom;
      switch (url.pathname) {
        case '/api/auth/me':
          return json(200, me);
        case '/api/labels':
          return json(200, LABELS);
        case '/api/counts':
          return json(200, { inbox_unread: 0, drafts: 0, scheduled: 0, spam_unread: 0, labels: {} });
        case '/api/threads':
          return json(200, page([item(1, { subject: '季度周报' })], { total: 1 }));
        case '/api/threads/1':
          return json(200, thread1);
        case '/api/threads/actions':
          return json(200, { thread_ids: JSON.parse(String(init.body)).thread_ids });
        default:
          return json(404, { error: { code: 'not_found', message: '内容不存在或已被删除', details: {} } });
      }
    }),
  );
}

function renderAt(path: string) {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const router = createMemoryRouter(routes, { initialEntries: [path] });
  render(
    <QueryClientProvider client={qc}>
      <TooltipProvider>
        <RouterProvider router={router} />
      </TooltipProvider>
    </QueryClientProvider>,
  );
  return { router, qc };
}

const alice = { name: 'Alice', email: 'alice@team.test' };
const fromZs = (over: Partial<Message>) => message(1, { subject: '季度周报', ...over });

beforeEach(() => {
  calls.length = 0;
  me = ME;
  thread1 = detail(1, [fromZs({ id: 10, text: '请查收', snippet: '请查收' })], { subject: '季度周报' });
  vi.stubGlobal('WebSocket', QuietSocket);
  vi.stubGlobal('ResizeObserver', NoopResizeObserver);
  setToken('tok');
});
afterEach(() => {
  clearToken();
  useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null, ownerId: null });
  useToastStore.getState().clear();
  useUiStore.setState({ list: null, cursorId: null, pages: {}, pagesPageSize: null, selected: [], selectionScope: null, shortcutsHelpOpen: false });
});

async function openThread1() {
  const r = renderAt('/mail/inbox/1');
  expect(await screen.findByRole('heading', { name: /季度周报/ }, { timeout: 4000 })).toBeInTheDocument();
  return r;
}

describe('thread view', () => {
  it('F2: the reply just sent from a draft of this thread (same id) is expanded as the newest message', async () => {
    thread1 = detail(1, [fromZs({ id: 10 }), fromZs({ id: 11, is_draft: true, direction: 'out', from: alice, snippet: '好的', text: '好的，收到' })], { subject: '季度周报' });
    api();
    const { qc } = await openThread1();
    expect(screen.getAllByRole('article')).toHaveLength(1);
    expect(screen.getByRole('button', { name: /草稿：好的/ })).toBeInTheDocument();

    // Sent: the backend turns the draft row into the sent message (same id, read).
    thread1 = detail(1, [fromZs({ id: 10 }), fromZs({ id: 11, direction: 'out', from: alice, snippet: '好的', text: '好的，收到', is_read: true })], { subject: '季度周报' });
    await act(() => qc.invalidateQueries({ queryKey: queryKeys.thread(1) }));
    await waitFor(() => expect(screen.getAllByRole('article')).toHaveLength(2));
    expect(screen.getAllByRole('article')[1]).toHaveTextContent('好的，收到');
  });

  it('F10: a message that arrives in the open thread is marked read', async () => {
    thread1 = detail(1, [fromZs({ id: 10, is_read: false })], { subject: '季度周报' });
    api();
    const { qc } = await openThread1();
    await waitFor(() => expect(actionCalls()).toEqual([{ thread_ids: [1], action: 'read' }]));
    thread1 = detail(1, [fromZs({ id: 10 }), fromZs({ id: 12, from: { name: '李四', email: 'ls@team.test' }, is_read: false, text: '补充一下' })], { subject: '季度周报' });
    await act(() => qc.invalidateQueries({ queryKey: queryKeys.thread(1) }));
    expect(await screen.findByText('补充一下')).toBeInTheDocument();
    await waitFor(() => expect(actionCalls()).toHaveLength(2));
    expect(actionCalls()[1]).toEqual({ thread_ids: [1], action: 'read' });
  });

  it('F5: collapsed messages keep their snippet on narrow screens (no `hidden … sm:block`)', async () => {
    thread1 = detail(1, [1, 2, 3].map((n) => fromZs({ id: 20 + n, snippet: `摘要 ${n}`, text: `正文 ${n}` })), { subject: '季度周报' });
    api();
    await openThread1();
    const snippets = await screen.findAllByTestId('message-snippet');
    expect(snippets.map((s) => s.textContent)).toEqual(['摘要 1', '摘要 2']);
    for (const s of snippets) expect(s.className.split(/\s+/)).not.toContain('hidden');
  });

  it('spec F5: 更改发送时间 opens the schedule picker and reschedules the message; errors are shown', async () => {
    const at = Date.now() + 2 * 86_400_000;
    const scheduled = (scheduledAt: number) =>
      fromZs({
        id: 10,
        direction: 'out',
        from: alice,
        outbound: { id: 3, status: 'scheduled', status_detail: null, scheduled_at: scheduledAt, scheduled_via: 'resend', undo_until: null, sent_at: null },
      });
    thread1 = detail(1, [scheduled(at)], { subject: '季度周报' });
    let reply: (body: { scheduled_at: number }) => Response = (body) => json(200, scheduled(body.scheduled_at));
    api((url, init) => (url.pathname === '/api/messages/10/reschedule' ? reply(JSON.parse(String(init.body))) : undefined));
    await openThread1();

    fireEvent.click(await screen.findByRole('button', { name: '更改发送时间' }));
    let dialog = await screen.findByRole('dialog', { name: '更改发送时间' });
    expect(within(dialog).getByText(/当前发送时间：/)).toBeInTheDocument();
    fireEvent.click(within(dialog).getByRole('button', { name: /明天下午 1:00/ }));
    await waitFor(() => expect(calls.some((c) => c.url.pathname === '/api/messages/10/reschedule')).toBe(true));
    const sent = JSON.parse(String(calls.find((c) => c.url.pathname === '/api/messages/10/reschedule')!.init.body));
    expect(sent.scheduled_at).toBeGreaterThan(Date.now());
    await waitFor(() => expect(useToastStore.getState().toasts.at(-1)?.message).toMatch(/^已将发送时间改为 /));
    expect(screen.queryByRole('dialog', { name: '更改发送时间' })).not.toBeInTheDocument();

    reply = () => json(409, { error: { code: 'already_sent', message: '邮件已发送', details: {} } });
    fireEvent.click(screen.getByRole('button', { name: '更改发送时间' }));
    dialog = await screen.findByRole('dialog', { name: '更改发送时间' });
    fireEvent.click(within(dialog).getByRole('button', { name: /明天上午 8:00/ }));
    await waitFor(() =>
      expect(useToastStore.getState().toasts.at(-1)).toMatchObject({ message: '邮件已发出，无法更改发送时间', tone: 'error' }),
    );
  });
});

describe('thread list pager', () => {
  it('F6: a double click on 较旧 while the next page loads pushes its cursor once', async () => {
    let releasePage2!: () => void;
    const page2 = new Promise<void>((r) => (releasePage2 = r));
    const rows = (from: number) => Array.from({ length: 50 }, (_, i) => item(from + i, { subject: `会话 ${from + i}` }));
    api((url) => {
      if (url.pathname !== '/api/threads') return undefined;
      if (url.searchParams.get('cursor') === 'C1') return page2.then(() => json(200, page(rows(51), { total: 120, next_cursor: 'C2' })));
      return json(200, page(rows(1), { total: 120, next_cursor: 'C1' }));
    });
    renderAt('/mail/inbox');
    await screen.findByText('会话 1');
    const older = screen.getByRole('button', { name: '较旧' });
    fireEvent.click(older);
    fireEvent.click(older);
    expect(useUiStore.getState().pages['folder:inbox']).toEqual(['C1']);
    releasePage2();
    expect(await screen.findByText('会话 51')).toBeInTheDocument();
    expect(screen.getByText('第 51-100 行，共 120 行')).toBeInTheDocument();
    expect(useUiStore.getState().pages['folder:inbox']).toEqual(['C1']);
  });

  it('F11: changing 每页显示 starts over at page 1 with the new size', async () => {
    api((url) => {
      if (url.pathname !== '/api/threads') return undefined;
      const limit = Number(url.searchParams.get('limit'));
      const from = url.searchParams.get('cursor') === 'C1' ? 51 : 1;
      return json(200, page(Array.from({ length: limit }, (_, i) => item(from + i, { subject: `会话 ${from + i}` })), { total: 120, next_cursor: 'C1' }));
    });
    const { qc } = renderAt('/mail/inbox');
    await screen.findByText('会话 1');
    fireEvent.click(screen.getByRole('button', { name: '较旧' }));
    expect(await screen.findByText('第 51-100 行，共 120 行')).toBeInTheDocument();

    act(() => qc.setQueryData<Me>(queryKeys.me(), { ...ME, settings: { ...ME.settings, page_size: 25 } }));
    expect(await screen.findByText('第 1-25 行，共 120 行')).toBeInTheDocument();
    expect(useUiStore.getState().pages['folder:inbox'] ?? []).toEqual([]);
    const last = calls.filter((c) => c.url.pathname === '/api/threads').at(-1)!;
    expect(last.url.searchParams.get('limit')).toBe('25');
    expect(last.url.searchParams.get('cursor')).toBeNull();
  });
});

describe('routes', () => {
  it('F17: /mail/search/:id without ?q= opens the thread under 所有邮件 instead of the inbox', async () => {
    api();
    const { router } = renderAt('/mail/search/1');
    await waitFor(() => expect(router.state.location.pathname).toBe('/mail/all/1'));
    expect(await screen.findByRole('heading', { name: /季度周报/ }, { timeout: 4000 })).toBeInTheDocument();
  });
});

describe('label dialog (mail side)', () => {
  it('F14: rejects names over the server limit of 64 characters (code points) before calling the API', async () => {
    api((url, init) => (url.pathname === '/api/labels' && init.method === 'POST' ? json(201, { id: 9, name: 'x', color: '#1a73e8', sort_order: 2 }) : undefined));
    const qc = new QueryClient();
    render(
      <QueryClientProvider client={qc}>
        <TooltipProvider>
          <LabelDialogHost />
        </TooltipProvider>
      </QueryClientProvider>,
    );
    act(() => openLabelDialog({ mode: 'create' }));
    const input = await screen.findByLabelText('标签名称');
    fireEvent.change(input, { target: { value: 'x'.repeat(65) } });
    fireEvent.click(screen.getByRole('button', { name: '创建' }));
    expect(await screen.findByText('标签名称不能超过 64 个字符')).toBeInTheDocument();
    expect(calls.some((c) => c.url.pathname === '/api/labels' && c.init.method === 'POST')).toBe(false);
    // 64 emoji = 128 UTF-16 units but 64 characters: accepted.
    fireEvent.change(input, { target: { value: '😀'.repeat(64) } });
    fireEvent.click(screen.getByRole('button', { name: '创建' }));
    await waitFor(() => expect(calls.some((c) => c.url.pathname === '/api/labels' && c.init.method === 'POST')).toBe(true));
  });
});
