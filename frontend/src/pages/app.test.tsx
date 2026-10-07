/**
 * Route-level tests: RequireAuth, LoginPage, MailPage (folder / label / search validation,
 * list, thread view), against a stubbed fetch and a fake WebSocket.
 */
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ThreadListResponse } from '@/api/types';
import { TooltipProvider } from '@/components/common';
import { detail, item, LABELS, ME, message, page } from '@/components/mail/testFixtures';
import { routes } from '@/router';
import { clearToken, setToken } from '@/stores/auth';
import { useComposeStore } from '@/stores/compose';
import { useToastStore } from '@/stores/toast';
import { useUiStore } from '@/stores/ui';
import { formatWait, safeNext } from './LoginPage';
import { parseId } from './MailPage';

class QuietSocket {
  readyState = 0;
  onopen: (() => void) | null = null;
  onmessage: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: (() => void) | null = null;
  send() {}
  close() {}
}

type Handler = (url: URL, init: RequestInit) => Response | undefined;
const json = (status: number, body: unknown) => new Response(body === undefined ? null : JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
const calls: { url: URL; init: RequestInit }[] = [];

function api(extra: Handler = () => undefined) {
  const inbox: ThreadListResponse = page([item(1, { subject: '季度周报', unread: true }), item(2, { subject: '项目同步' })], { total: 2 });
  vi.stubGlobal(
    'fetch',
    vi.fn(async (input: string, init: RequestInit = {}) => {
      const url = new URL(input, 'http://localhost');
      calls.push({ url, init });
      const custom = extra(url, init);
      if (custom) return custom;
      switch (url.pathname) {
        case '/api/auth/me':
          return json(200, ME);
        case '/api/labels':
          return json(200, LABELS);
        case '/api/counts':
          return json(200, { inbox_unread: 1, drafts: 0, scheduled: 0, spam_unread: 0, labels: {} });
        case '/api/threads':
          return json(200, inbox);
        case '/api/threads/1':
          return json(200, detail(1, [message(1, { id: 10, subject: '季度周报', text: '请查收', is_read: false })], { subject: '季度周报' }));
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

/** jsdom has no ResizeObserver (Radix uses it for checkboxes inside forms). */
class NoopResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

beforeEach(() => {
  calls.length = 0;
  vi.stubGlobal('WebSocket', QuietSocket);
  vi.stubGlobal('ResizeObserver', NoopResizeObserver);
});
afterEach(() => {
  clearToken();
  useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null, ownerId: null });
  useToastStore.getState().clear();
  useUiStore.setState({ list: null, cursorId: null, pages: {}, selected: [], selectionScope: null, shortcutsHelpOpen: false });
});

describe('route helpers', () => {
  it('only follows same-origin next paths', () => {
    expect(safeNext('/mail/sent/3?x=1')).toBe('/mail/sent/3?x=1');
    expect(safeNext(null)).toBe('/mail/inbox');
    expect(safeNext('https://evil.test')).toBe('/mail/inbox');
    expect(safeNext('//evil.test')).toBe('/mail/inbox');
    expect(safeNext('/\\evil.test')).toBe('/mail/inbox');
    expect(safeNext('/login?next=/x')).toBe('/mail/inbox');
    expect(safeNext('/mail\n/x')).toBe('/mail/inbox');
    expect(formatWait(30)).toBe('30 秒');
    expect(formatWait(75)).toBe('2 分钟');
    expect(parseId('42')).toBe(42);
    expect(parseId('0')).toBeNull();
    expect(parseId('4x')).toBeNull();
    expect(parseId(undefined)).toBeNull();
  });
});

describe('RequireAuth', () => {
  it('sends anonymous visitors to /login and keeps deep links in ?next', async () => {
    api();
    const { router } = renderAt('/mail/sent/5');
    expect(await screen.findByRole('heading', { name: '登录' })).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/login');
    expect(router.state.location.search).toBe(`?next=${encodeURIComponent('/mail/sent/5')}`);
  });

  it('redirects / to the inbox and renders the list', async () => {
    setToken('tok');
    api();
    const { router } = renderAt('/');
    expect(await screen.findByText('季度周报')).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/mail/inbox');
    expect(screen.getByText('项目同步')).toBeInTheDocument();
    expect(screen.getByText('第 1-2 行，共 2 行')).toBeInTheDocument();
    await waitFor(() => expect(document.title).toBe('收件箱 (1) - alice@team.test - AZ Mail'));
    const list = calls.find((c) => c.url.pathname === '/api/threads')!;
    expect(list.url.searchParams.get('folder')).toBe('inbox');
    expect(list.url.searchParams.get('limit')).toBe('50');
    expect(list.url.searchParams.get('tzoff')).not.toBeNull();
    // Sidebar with counts and labels.
    const nav = screen.getByRole('navigation', { name: '主菜单' });
    expect(within(nav).getByRole('link', { name: /收件箱/ })).toHaveAttribute('aria-current', 'page');
    expect(within(nav).getByText('工作')).toBeInTheDocument();
  });
});

describe('MailPage', () => {
  beforeEach(() => setToken('tok'));

  it('rejects unknown folders, bad ids and missing labels with NotFound', async () => {
    api();
    renderAt('/mail/bogus');
    expect(await screen.findByText('找不到该页面')).toBeInTheDocument();
  });

  it('rejects a non-numeric thread id', async () => {
    api();
    renderAt('/mail/inbox/abc');
    expect(await screen.findByText('找不到该页面')).toBeInTheDocument();
  });

  it('rejects a label that does not exist', async () => {
    api();
    renderAt('/mail/label/999');
    expect(await screen.findByText('找不到该页面')).toBeInTheDocument();
  });

  it('lists a label view with label_id', async () => {
    api();
    renderAt('/mail/label/1');
    expect(await screen.findByText('季度周报')).toBeInTheDocument();
    await waitFor(() => expect(calls.some((c) => c.url.pathname === '/api/threads' && c.url.searchParams.get('label_id') === '1')).toBe(true));
  });

  it('search without q goes back to the inbox; with q sends q', async () => {
    api();
    const { router } = renderAt('/mail/search');
    await waitFor(() => expect(router.state.location.pathname).toBe('/mail/inbox'));
    await act(() => router.navigate('/mail/search?q=from%3Abob%20%E5%91%A8%E6%8A%A5'));
    await waitFor(() => expect(calls.some((c) => c.url.searchParams.get('q') === 'from:bob 周报')).toBe(true));
    expect(screen.getByRole('textbox', { name: '搜索邮件' })).toHaveValue('from:bob 周报');
  });

  it('opens a thread, marks it read and goes back with u', async () => {
    api();
    const { router } = renderAt('/mail/inbox/1');
    expect(await screen.findByRole('heading', { name: /季度周报/ }, { timeout: 4000 })).toBeInTheDocument();
    expect(screen.getByText('请查收')).toBeInTheDocument();
    await waitFor(() => {
      const action = calls.find((c) => c.url.pathname === '/api/threads/actions');
      expect(action && JSON.parse(String(action.init.body))).toEqual({ thread_ids: [1], action: 'read' });
    });
    fireEvent.keyDown(document.body, { key: 'u' });
    await waitFor(() => expect(router.state.location.pathname).toBe('/mail/inbox'));
  });

  it('shows the not-found state for a thread the server does not know', async () => {
    api();
    renderAt('/mail/inbox/77');
    expect(await screen.findByText('找不到此会话，它可能已被删除。')).toBeInTheDocument();
  });

  it('mounts the lazy settings and admin pages [F] inside the shell', async () => {
    api();
    const { router } = renderAt('/settings');
    expect(await screen.findByTestId('settings-page')).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/settings/general');
    expect(screen.getByRole('navigation', { name: '主菜单' })).toBeInTheDocument();
    await act(() => router.navigate('/admin'));
    expect(await screen.findByTestId('admin-layout')).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/admin/users');
  });

  it('c opens a compose window; ? opens the shortcuts help', async () => {
    api();
    renderAt('/mail/inbox');
    await screen.findByText('季度周报');
    fireEvent.keyDown(document.body, { key: 'c' });
    expect(useComposeStore.getState().windows.map((w) => w.init.kind)).toEqual(['new']);
    fireEvent.keyDown(document.body, { key: '?', shiftKey: true });
    expect(await screen.findByRole('dialog', { name: '键盘快捷键' })).toBeInTheDocument();
  });
});

describe('LoginPage', () => {
  function fill(email: string, password: string) {
    fireEvent.change(screen.getByLabelText('邮箱地址'), { target: { value: email } });
    fireEvent.change(screen.getByLabelText('密码'), { target: { value: password } });
    fireEvent.click(screen.getByRole('button', { name: '登录' }));
  }

  it('validates required fields', async () => {
    api();
    renderAt('/login');
    fireEvent.click(await screen.findByRole('button', { name: '登录' }));
    expect(await screen.findByText('请输入邮箱地址')).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText('邮箱地址'), { target: { value: 'a@b.test' } });
    fireEvent.click(screen.getByRole('button', { name: '登录' }));
    expect(await screen.findByText('请输入密码')).toBeInTheDocument();
  });

  it('shows invalid_credentials, account_disabled and too_many_attempts', async () => {
    let reply: Response = json(401, { error: { code: 'invalid_credentials', message: '邮箱或密码错误', details: {} } });
    api((url) => (url.pathname === '/api/auth/login' ? reply.clone() : undefined));
    renderAt('/login');
    await screen.findByRole('heading', { name: '登录' });
    fill('a@b.test', 'wrong');
    expect(await screen.findByText('邮箱或密码错误')).toBeInTheDocument();
    expect(screen.getByLabelText('密码')).toHaveValue('');

    reply = json(403, { error: { code: 'account_disabled', message: '账号已停用', details: {} } });
    fill('a@b.test', 'x');
    expect(await screen.findByText('此账号已被停用，请联系管理员')).toBeInTheDocument();

    reply = json(429, { error: { code: 'too_many_attempts', message: '尝试次数过多', details: { retry_after: 120 } } });
    fill('a@b.test', 'x');
    expect(await screen.findByText('尝试次数过多，请在 2 分钟后重试')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '登录' })).toBeDisabled();
  });

  it('logs in and follows ?next', async () => {
    api((url) => (url.pathname === '/api/auth/login' ? json(200, { token: 'tok', expires_at: 0, user: ME }) : undefined));
    const { router } = renderAt(`/login?next=${encodeURIComponent('/mail/inbox/1')}`);
    await screen.findByRole('heading', { name: '登录' });
    fill(' alice@team.test ', 'secret');
    await waitFor(() => expect(router.state.location.pathname).toBe('/mail/inbox/1'));
    const login = calls.find((c) => c.url.pathname === '/api/auth/login')!;
    expect(JSON.parse(String(login.init.body))).toEqual({ email: 'alice@team.test', password: 'secret' });
    expect(await screen.findByRole('heading', { name: /季度周报/ }, { timeout: 4000 })).toBeInTheDocument();
  });
});
