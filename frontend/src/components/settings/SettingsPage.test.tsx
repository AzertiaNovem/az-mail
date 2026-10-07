import { QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '@/api/client';
import * as endpoints from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Me, Settings } from '@/api/types';
import { TooltipProvider } from '@/components/common';
import { installEditorDomPolyfills, makeMe, makeQueryClient } from '@/components/compose/testFixtures';
import { SettingsPage } from '@/pages/SettingsPage';
import { toast, useToastStore } from '@/stores/toast';

installEditorDomPolyfills();

vi.mock('@/api/endpoints', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/endpoints')>()),
  updateSettings: vi.fn(),
  changePassword: vi.fn(),
  listLabels: vi.fn(async () => [
    { id: 1, name: '工作', color: '#4a86e8', sort_order: 0 },
    { id: 2, name: '家庭', color: '#16a766', sort_order: 1 },
  ]),
  getCounts: vi.fn(async () => ({ inbox_unread: 0, drafts: 0, scheduled: 0, spam_unread: 0, labels: { '1': { unread: 1, total: 12 } } })),
  createLabel: vi.fn(async (input: { name: string; color: string }) => ({ id: 3, sort_order: 2, ...input })),
  updateLabel: vi.fn(),
  deleteLabel: vi.fn(async () => undefined),
}));
const api = vi.mocked(endpoints);

afterEach(() => {
  act(() => toast.clear());
  vi.clearAllMocks();
});

function mount(tab: string, me?: Me) {
  const qc = makeQueryClient(me ?? makeMe({ settings: { ...makeMe().settings, trusted_image_senders: ['news@ext.test'] } }));
  const router = createMemoryRouter(
    [
      { path: '/settings/:tab', element: <SettingsPage /> },
      { path: '*', element: <p>elsewhere</p> },
    ],
    { initialEntries: [`/settings/${tab}`] },
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

describe('SettingsPage', () => {
  it('rejects unknown tabs', () => {
    mount('nope');
    expect(screen.getByText('找不到该设置页面')).toBeInTheDocument();
    expect(screen.getAllByRole('link', { name: '常规' })).toHaveLength(2); // tab + fallback link
  });
});

describe('Settings → 常规', () => {
  it('validates, saves only the changed fields and updates the cached Me', async () => {
    const { qc } = mount('general');
    const name = screen.getByLabelText('显示名称');
    const save = screen.getByRole('button', { name: '保存更改' });
    expect(save).toBeDisabled();

    fireEvent.change(name, { target: { value: '  ' } });
    fireEvent.click(save);
    expect(await screen.findByText('请输入显示名称')).toBeInTheDocument();
    expect(api.updateSettings).not.toHaveBeenCalled();

    fireEvent.change(name, { target: { value: '张三丰' } });
    fireEvent.change(screen.getByLabelText('撤销发送'), { target: { value: '30' } });
    fireEvent.click(screen.getByLabelText('始终显示外部图片'));
    fireEvent.click(screen.getByRole('button', { name: '移除 news@ext.test' }));
    const saved: Settings = { ...makeMe().settings, display_name: '张三丰', undo_send_seconds: 30, remote_images: 'always', trusted_image_senders: [] };
    api.updateSettings.mockResolvedValue(saved);
    fireEvent.click(save);
    await waitFor(() => expect(api.updateSettings).toHaveBeenCalledTimes(1));
    expect(api.updateSettings.mock.calls[0]![0]).toEqual({
      display_name: '张三丰',
      undo_send_seconds: 30,
      remote_images: 'always',
      trusted_image_senders: [],
    });
    await waitFor(() => expect(qc.getQueryData<Me>(queryKeys.me())?.display_name).toBe('张三丰'));
    expect(qc.getQueryData<Me>(queryKeys.me())?.settings.undo_send_seconds).toBe(30);
    expect(useToastStore.getState().toasts.map((t) => t.message)).toContain('设置已保存');
    await waitFor(() => expect(save).toBeDisabled());
  });

  it('取消 restores the saved values', () => {
    mount('general');
    const name = screen.getByLabelText('显示名称');
    fireEvent.change(name, { target: { value: '临时' } });
    expect(screen.getByText('你有未保存的更改')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '取消' }));
    expect(name).toHaveValue('张三');
    expect(screen.getByRole('button', { name: '保存更改' })).toBeDisabled();
  });

  it('shows a save failure', async () => {
    api.updateSettings.mockRejectedValue(new ApiError(500, 'internal_error', '服务器内部错误'));
    mount('general');
    fireEvent.change(screen.getByLabelText('每页显示'), { target: { value: '100' } });
    fireEvent.click(screen.getByRole('button', { name: '保存更改' }));
    await waitFor(() => expect(useToastStore.getState().toasts[0]?.message).toBe('保存失败：服务器内部错误'));
  });

  it('asks before leaving with unsaved changes', async () => {
    const { router } = mount('general');
    fireEvent.change(screen.getByLabelText('显示名称'), { target: { value: '改了' } });
    act(() => void router.navigate('/mail/inbox'));
    const dialog = await screen.findByRole('dialog', { name: '放弃未保存的更改？' });
    fireEvent.click(within(dialog).getByRole('button', { name: '继续编辑' }));
    await waitFor(() => expect(router.state.location.pathname).toBe('/settings/general'));
    act(() => void router.navigate('/mail/inbox'));
    fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: '放弃更改' }));
    await waitFor(() => expect(router.state.location.pathname).toBe('/mail/inbox'));
  });
});

describe('Settings → 标签', () => {
  it('lists labels with counts and creates a label (validated)', async () => {
    mount('labels');
    expect(await screen.findByText('工作')).toBeInTheDocument();
    expect(await screen.findByText('12 个会话')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '新建标签' }));
    const dialog = await screen.findByRole('dialog', { name: '新建标签' });
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    expect(await within(dialog).findByText('请输入标签名称')).toBeInTheDocument();
    fireEvent.change(within(dialog).getByLabelText('标签名称'), { target: { value: '旅行' } });
    fireEvent.click(within(dialog).getByRole('radio', { name: '选择颜色 #fb4c2f' }));
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    await waitFor(() => expect(api.createLabel).toHaveBeenCalledWith({ name: '旅行', color: '#fb4c2f' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
  });

  it('shows label_exists from the server under the name', async () => {
    api.createLabel.mockRejectedValueOnce(new ApiError(409, 'label_exists', '已存在同名标签'));
    mount('labels');
    await screen.findByText('工作');
    fireEvent.click(screen.getByRole('button', { name: '新建标签' }));
    const dialog = await screen.findByRole('dialog', { name: '新建标签' });
    fireEvent.change(within(dialog).getByLabelText('标签名称'), { target: { value: '工作' } });
    fireEvent.click(within(dialog).getByRole('button', { name: '创建' }));
    expect(await within(dialog).findByText('已存在同名标签')).toBeInTheDocument();
  });

  it('deletes a label after confirmation', async () => {
    mount('labels');
    await screen.findByText('家庭');
    fireEvent.click(screen.getByRole('button', { name: '删除 家庭' }));
    const dialog = await screen.findByRole('dialog', { name: '删除标签“家庭”？' });
    fireEvent.click(within(dialog).getByRole('button', { name: '删除' }));
    await waitFor(() => expect(api.deleteLabel).toHaveBeenCalledWith(2));
  });
});

describe('Settings → 账号', () => {
  it('shows the account and validates the password form', async () => {
    mount('account');
    expect(screen.getByText('me@az.test')).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText('当前密码'), { target: { value: 'old-password' } });
    fireEvent.change(screen.getByLabelText('新密码'), { target: { value: 'new-password' } });
    fireEvent.change(screen.getByLabelText('确认新密码'), { target: { value: 'other-password' } });
    fireEvent.click(screen.getByRole('button', { name: '修改密码' }));
    expect(await screen.findByText('两次输入的新密码不一致')).toBeInTheDocument();
    expect(api.changePassword).not.toHaveBeenCalled();
  });

  it('maps 403 invalid_credentials and 422 weak_password to the fields', async () => {
    mount('account');
    fireEvent.change(screen.getByLabelText('当前密码'), { target: { value: 'wrong-pass' } });
    fireEvent.change(screen.getByLabelText('新密码'), { target: { value: 'new-password' } });
    fireEvent.change(screen.getByLabelText('确认新密码'), { target: { value: 'new-password' } });
    api.changePassword.mockRejectedValueOnce(new ApiError(403, 'invalid_credentials', '邮箱或密码错误'));
    fireEvent.click(screen.getByRole('button', { name: '修改密码' }));
    expect(await screen.findByText('当前密码不正确')).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText('当前密码'), { target: { value: 'right-pass' } });
    api.changePassword.mockRejectedValueOnce(new ApiError(422, 'weak_password', '密码太常见，请换一个'));
    fireEvent.click(screen.getByRole('button', { name: '修改密码' }));
    expect(await screen.findByText('密码太常见，请换一个')).toBeInTheDocument();

    api.changePassword.mockResolvedValueOnce(undefined);
    fireEvent.click(screen.getByRole('button', { name: '修改密码' }));
    await waitFor(() => expect(screen.getByLabelText('当前密码')).toHaveValue(''));
    expect(api.changePassword).toHaveBeenLastCalledWith({ current_password: 'right-pass', new_password: 'new-password' });
    expect(useToastStore.getState().toasts.map((t) => t.message)).toContain('密码已修改，其他设备上的登录已退出');
  });
});
