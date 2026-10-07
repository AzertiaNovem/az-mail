import { QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import * as admin from '@/api/admin';
import { ApiError } from '@/api/client';
import type { AdminAlias, AdminStats, AdminUser, Me } from '@/api/types';
import { TooltipProvider } from '@/components/common';
import { makeMe, makeQueryClient } from '@/components/compose/testFixtures';
import { AdminAliases } from '@/pages/admin/Aliases';
import { AdminDomains } from '@/pages/admin/Domains';
import { AdminEvents } from '@/pages/admin/Events';
import { AdminLayout } from '@/pages/admin/AdminLayout';
import { AdminOutbox } from '@/pages/admin/Outbox';
import { AdminStats as AdminStatsPage } from '@/pages/admin/Stats';
import { AdminUsers } from '@/pages/admin/Users';
import { toast, useToastStore } from '@/stores/toast';

vi.mock('@/api/admin', async (importOriginal) => {
  const orig = await importOriginal<typeof import('@/api/admin')>();
  const fns = [
    'adminListUsers',
    'adminCreateUser',
    'adminUpdateUser',
    'adminDeleteUser',
    'adminListAliases',
    'adminCreateAlias',
    'adminUpdateAlias',
    'adminDeleteAlias',
    'adminListDomains',
    'adminCreateDomain',
    'adminDeleteDomain',
    'adminGetDomainStatus',
    'adminListEvents',
    'adminListInbound',
    'adminListOutbox',
    'adminRetryOutbox',
    'adminListJobs',
    'adminRetryJob',
    'adminSync',
    'adminGetStats',
  ] as const;
  return { ...orig, ...Object.fromEntries(fns.map((f) => [f, vi.fn()])) };
});
const api = vi.mocked(admin);

const user = (p: Partial<AdminUser>): AdminUser => ({
  id: 1,
  email: 'me@az.test',
  display_name: '张三',
  is_admin: true,
  disabled: false,
  created_at: 0,
  last_login_at: null,
  message_count: 3,
  storage_bytes: 2048,
  aliases: [],
  ...p,
});

const adminMe = makeMe({ is_admin: true });

function mountAdmin(path: string, me: Me = adminMe) {
  const qc = makeQueryClient(me);
  const router = createMemoryRouter(
    [
      {
        path: '/admin',
        element: <AdminLayout />,
        children: [
          { path: 'users', element: <AdminUsers /> },
          { path: 'aliases', element: <AdminAliases /> },
          { path: 'domains', element: <AdminDomains /> },
          { path: 'events', element: <AdminEvents /> },
          { path: 'outbox', element: <AdminOutbox /> },
          { path: 'stats', element: <AdminStatsPage /> },
        ],
      },
      { path: '*', element: <p>mail</p> },
    ],
    { initialEntries: [path] },
  );
  render(
    <QueryClientProvider client={qc}>
      <TooltipProvider>
        <RouterProvider router={router} />
      </TooltipProvider>
    </QueryClientProvider>,
  );
  return { qc, router };
}

beforeEach(() => {
  api.adminListUsers.mockResolvedValue([
    user({ id: 1 }),
    user({ id: 2, email: 'li@az.test', display_name: '李四', is_admin: false, disabled: true, last_login_at: Date.UTC(2026, 9, 7, 12, 3) }),
  ]);
  api.adminListDomains.mockResolvedValue([{ id: 1, name: 'az.test', receiving_enabled: true, created_at: 0 }]);
});

afterEach(() => {
  act(() => toast.clear());
  vi.clearAllMocks();
});

describe('AdminLayout', () => {
  it('blocks non-admins', () => {
    mountAdmin('/admin/users', makeMe({ is_admin: false }));
    expect(screen.getByText('你没有访问管理后台的权限')).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: '管理分类' })).not.toBeInTheDocument();
    expect(api.adminListUsers).not.toHaveBeenCalled();
  });

  it('shows the tab bar for admins', async () => {
    mountAdmin('/admin/users');
    const nav = screen.getByRole('navigation', { name: '管理分类' });
    expect(within(nav).getAllByRole('link').map((a) => a.textContent)).toEqual(['用户', '别名', '域名', 'Webhook 事件', '发件队列', '统计']);
    expect(await screen.findByText('李四')).toBeInTheDocument();
  });
});

describe('Admin → 用户', () => {
  it('renders the table and forbids deleting yourself', async () => {
    mountAdmin('/admin/users');
    const row = (await screen.findByText('李四')).closest('tr')!;
    expect(within(row).getByText('已停用')).toBeInTheDocument();
    expect(within(row).getByText('2026-10-07 20:03')).toBeInTheDocument();
    expect(screen.getByText('从未')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '不能删除自己的账号' })).toBeDisabled();
  });

  it('create dialog validates locally and maps server errors to fields', async () => {
    api.adminCreateUser.mockRejectedValueOnce(new ApiError(409, 'address_exists', '该邮箱地址已被使用'));
    api.adminCreateUser.mockResolvedValueOnce(user({ id: 3, email: 'ww@az.test' }));
    mountAdmin('/admin/users');
    await screen.findByText('李四');
    fireEvent.click(screen.getByRole('button', { name: '创建用户' }));
    const dialog = await screen.findByRole('dialog', { name: '创建用户' });
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    expect(within(dialog).getByText('请输入邮箱地址')).toBeInTheDocument();
    expect(within(dialog).getByText('请输入显示名称')).toBeInTheDocument();
    expect(within(dialog).getByText('请输入初始密码')).toBeInTheDocument();
    expect(api.adminCreateUser).not.toHaveBeenCalled();

    fireEvent.change(within(dialog).getByLabelText('邮箱地址'), { target: { value: 'WW' } });
    fireEvent.change(within(dialog).getByLabelText('显示名称'), { target: { value: '王五' } });
    fireEvent.change(within(dialog).getByLabelText('初始密码'), { target: { value: 'secret-123' } });
    fireEvent.click(within(dialog).getByRole('switch', { name: '管理员' }));
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    expect(await within(dialog).findByText('该邮箱地址已被使用')).toBeInTheDocument();
    expect(api.adminCreateUser).toHaveBeenCalledWith({ email: 'ww@az.test', display_name: '王五', password: 'secret-123', is_admin: true });

    fireEvent.change(within(dialog).getByLabelText('邮箱地址'), { target: { value: 'ww' } });
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(useToastStore.getState().toasts.map((t) => t.message)).toContain('已创建用户 ww@az.test');
  });

  it('edit dialog sends only changed fields and shows last_admin', async () => {
    api.adminUpdateUser.mockRejectedValueOnce(new ApiError(409, 'last_admin', '至少需要保留一位管理员'));
    mountAdmin('/admin/users');
    await screen.findByText('李四');
    fireEvent.click(screen.getByRole('button', { name: '修改 me@az.test' }));
    const dialog = await screen.findByRole('dialog', { name: '修改用户 me@az.test' });
    expect(within(dialog).getByRole('switch', { name: '停用账号' })).toBeDisabled(); // yourself
    expect(within(dialog).getByRole('button', { name: '保存' })).toBeDisabled();
    fireEvent.click(within(dialog).getByRole('switch', { name: '管理员' }));
    fireEvent.click(within(dialog).getByRole('button', { name: '保存' }));
    await waitFor(() => expect(api.adminUpdateUser).toHaveBeenCalledWith(1, { is_admin: false }));
    expect(await within(dialog).findByText('至少需要保留一位管理员')).toBeInTheDocument();
  });

  it('delete asks first and reports errors', async () => {
    api.adminDeleteUser.mockRejectedValueOnce(new ApiError(409, 'last_admin', '至少需要保留一位管理员'));
    mountAdmin('/admin/users');
    await screen.findByText('李四');
    fireEvent.click(screen.getByRole('button', { name: '删除 li@az.test' }));
    const dialog = await screen.findByRole('dialog', { name: '删除用户 li@az.test？' });
    fireEvent.click(within(dialog).getByRole('button', { name: '删除' }));
    await waitFor(() => expect(api.adminDeleteUser).toHaveBeenCalledWith(2));
    await waitFor(() => expect(useToastStore.getState().toasts[0]?.message).toBe('删除失败：至少需要保留一位管理员'));
  });
});

describe('Admin → 别名', () => {
  const alias: AdminAlias = {
    id: 9,
    email: 'support@az.test',
    display_name: '客服',
    share_sent: true,
    created_at: 0,
    members: [{ user_id: 1, email: 'me@az.test', display_name: '张三', can_send_as: true }],
  };

  it('edits members (add, toggle send-as, remove) and sends the whole list', async () => {
    api.adminListAliases.mockResolvedValue([alias]);
    api.adminUpdateAlias.mockResolvedValue(alias);
    mountAdmin('/admin/aliases');
    await screen.findByText('support@az.test');
    fireEvent.click(screen.getByRole('button', { name: '修改 support@az.test' }));
    const dialog = await screen.findByRole('dialog', { name: '修改别名 support@az.test' });
    fireEvent.click(within(dialog).getByRole('button', { name: '选择成员' }));
    fireEvent.click(await screen.findByRole('button', { name: /李四/ }));
    fireEvent.click(within(dialog).getByRole('switch', { name: '可代发 me@az.test' }));
    fireEvent.click(within(dialog).getByRole('button', { name: '保存' }));
    await waitFor(() =>
      expect(api.adminUpdateAlias).toHaveBeenCalledWith(9, {
        email: 'support@az.test',
        display_name: '客服',
        share_sent: true,
        members: [
          { user_id: 1, can_send_as: false },
          { user_id: 2, can_send_as: true },
        ],
      }),
    );
  });

  it('explains alias_in_use on delete', async () => {
    api.adminListAliases.mockResolvedValue([alias]);
    api.adminDeleteAlias.mockRejectedValue(new ApiError(409, 'alias_in_use', 'x'));
    mountAdmin('/admin/aliases');
    await screen.findByText('support@az.test');
    fireEvent.click(screen.getByRole('button', { name: '删除 support@az.test' }));
    fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: '删除' }));
    await waitFor(() => expect(useToastStore.getState().toasts[0]?.message).toContain('此别名已用于发送邮件'));
  });
});

describe('Admin → 域名', () => {
  it('validates and adds a domain, and shows DNS records with copy buttons', async () => {
    api.adminCreateDomain.mockResolvedValue({ id: 2, name: 'new.test', receiving_enabled: true, created_at: 0 });
    api.adminGetDomainStatus.mockResolvedValue({
      id: 1,
      name: 'az.test',
      resend: {
        id: 'r1',
        status: 'pending',
        region: 'us-east-1',
        created_at: null,
        records: [{ record: 'DKIM', name: 'resend._domainkey', type: 'TXT', ttl: 'Auto', status: 'verified', value: 'p=MIGf' }],
      },
    });
    mountAdmin('/admin/domains');
    await screen.findByText('az.test');
    const input = screen.getByRole('textbox', { name: '域名' });
    fireEvent.change(input, { target: { value: 'bad domain' } });
    fireEvent.click(screen.getByRole('button', { name: '添加域名' }));
    expect(screen.getByText('请输入有效的域名，例如 example.com')).toBeInTheDocument();
    fireEvent.change(input, { target: { value: 'New.Test' } });
    fireEvent.click(screen.getByRole('button', { name: '添加域名' }));
    await waitFor(() => expect(api.adminCreateDomain).toHaveBeenCalledWith({ name: 'new.test' }));

    fireEvent.click(screen.getByRole('button', { name: '查看 DNS 状态' }));
    const dialog = await screen.findByRole('dialog', { name: 'az.test 的 DNS 状态' });
    expect(await within(dialog).findByText('p=MIGf')).toBeInTheDocument();
    expect(within(dialog).getByText('验证中')).toBeInTheDocument();
    expect(within(dialog).getByText('已验证')).toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: '复制值' })).toBeInTheDocument();
  });

  it('tells when Resend does not know the domain', async () => {
    api.adminGetDomainStatus.mockResolvedValue({ id: 1, name: 'az.test', resend: null });
    mountAdmin('/admin/domains');
    await screen.findByText('az.test');
    fireEvent.click(screen.getByRole('button', { name: '查看 DNS 状态' }));
    expect(await screen.findByText(/Resend 中找不到此域名/)).toBeInTheDocument();
  });
});

describe('Admin → Webhook 事件', () => {
  it('filters by type and pages with the cursor', async () => {
    const ev = (id: number, result: string | null) => ({
      id,
      svix_id: `msg_${id}`,
      type: 'email.delivered',
      resend_email_id: `e${id}`,
      received_at: 0,
      processed_at: null,
      result,
    });
    api.adminListEvents.mockImplementation(async (params) =>
      params?.cursor ? { items: [ev(3, 'error: boom')], next_cursor: null } : { items: [ev(1, 'applied'), ev(2, null)], next_cursor: 'c2' },
    );
    mountAdmin('/admin/events');
    expect(await screen.findByText('msg_1')).toBeInTheDocument();
    expect(screen.getByText('已应用')).toBeInTheDocument();
    expect(screen.getByText('未处理')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '下一页' }));
    expect(await screen.findByText('msg_3')).toBeInTheDocument();
    expect(screen.getByText('错误')).toBeInTheDocument();
    expect(screen.getByText('第 2 页')).toBeInTheDocument();
    expect(api.adminListEvents).toHaveBeenLastCalledWith({ type: undefined, cursor: 'c2' }, expect.anything());
    fireEvent.click(screen.getByRole('button', { name: '上一页' }));
    expect(await screen.findByText('msg_1')).toBeInTheDocument();

    fireEvent.change(screen.getByRole('combobox', { name: '类型' }), { target: { value: 'email.bounced' } });
    await waitFor(() => expect(api.adminListEvents).toHaveBeenLastCalledWith({ type: 'email.bounced', cursor: null }, expect.anything()));
  });
});

describe('Admin → 发件队列', () => {
  it('retries a failed send (new row) and switches filters / sections', async () => {
    api.adminListOutbox.mockImplementation(async (status) =>
      status === 'failed'
        ? [
            {
              id: 41,
              uuid: 'u',
              sender_user_id: 1,
              sender_email: 'me@az.test',
              from_email: 'support@az.test',
              status: 'failed',
              status_detail: '发送配额已用完',
              error_name: 'daily_quota_exceeded',
              scheduled_at: null,
              scheduled_via: null,
              resend_id: null,
              total_bytes: 2048,
              last_event: null,
              last_event_at: null,
              created_at: 0,
              updated_at: 0,
            },
          ]
        : [],
    );
    api.adminRetryOutbox.mockResolvedValue({ id: 42 } as never);
    api.adminListInbound.mockResolvedValue([]);
    api.adminListJobs.mockResolvedValue([]);
    const { qc } = mountAdmin('/admin/outbox');
    const spy = vi.spyOn(qc, 'invalidateQueries');
    expect(await screen.findByText('发送配额已用完')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '重试' }));
    await waitFor(() => expect(api.adminRetryOutbox).toHaveBeenCalledWith(41));
    await waitFor(() => expect(useToastStore.getState().toasts[0]?.message).toBe('已重新加入发送队列（新编号 #42）'));
    expect(spy).toHaveBeenCalledWith({ queryKey: ['admin', 'outbox'] });

    fireEvent.click(screen.getByRole('button', { name: '排队中' }));
    expect(await screen.findByText('没有符合条件的发件记录')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: '收件异常' }));
    expect(await screen.findByText('没有收件异常')).toBeInTheDocument();
    expect(api.adminListInbound).toHaveBeenCalledWith('unroutable', expect.anything());
    fireEvent.click(screen.getByRole('tab', { name: '失败任务' }));
    expect(await screen.findByText('没有失败的任务')).toBeInTheDocument();
    expect(api.adminListJobs).toHaveBeenCalledWith('dead', expect.anything());
  });
});

describe('Admin → 统计', () => {
  const stats: AdminStats = {
    users: 3,
    messages: 1758,
    storage_bytes: 1024 * 1024,
    queue: { pending: 2, dead: 1 },
    sent_24h: 34,
    received_24h: 120,
    failed_24h: 1,
    last_webhook_at: null,
    last_poll_at: Date.UTC(2026, 9, 7, 12, 3, 9),
    quota_blocked: true,
    storage: { backend: 'r2', delivery: 'proxy', blob_count: 812, blob_bytes: 2 * 1024 * 1024 },
  };

  it('shows the cards, the quota banner, and starts a manual sync', async () => {
    api.adminGetStats.mockResolvedValue(stats);
    api.adminSync.mockResolvedValue({ job_id: 77 });
    mountAdmin('/admin/stats');
    expect(await screen.findByText('1,758')).toBeInTheDocument();
    expect(screen.getByRole('alert')).toHaveTextContent('Resend 发送配额已用完');
    expect(screen.getByText('Cloudflare R2')).toBeInTheDocument();
    expect(screen.getByText(/812 个文件 · 2 MB/)).toBeInTheDocument();
    expect(screen.getByText('2026-10-07 20:03:09')).toBeInTheDocument();
    expect(screen.getByText('从未')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '立即同步收件' }));
    await waitFor(() => expect(useToastStore.getState().toasts[0]?.message).toBe('已开始同步收件（任务 #77）'));
  });

  it('shows load errors with retry', async () => {
    api.adminGetStats.mockRejectedValue(new ApiError(500, 'internal_error', '服务器内部错误'));
    mountAdmin('/admin/stats');
    expect(await screen.findByText('加载失败：服务器内部错误')).toBeInTheDocument();
    api.adminGetStats.mockResolvedValue({ ...stats, quota_blocked: false });
    fireEvent.click(screen.getByRole('button', { name: '重试' }));
    expect(await screen.findByText('1,758')).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });
});
