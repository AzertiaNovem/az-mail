/**
 * Admin → 用户 [WP-F]: user table (名称, 邮箱, 管理员, 状态, 最近登录, 邮件数, 存储) with
 * 创建用户 / 修改 / 删除. Deleting yourself or the last admin is refused by the server
 * (409 cannot_delete_self / last_admin) and shown as an error toast.
 *
 * Contract: `export function AdminUsers()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { errorMessage } from '@/api/client';
import { adminDeleteUser, adminKeys, adminListDomains, adminListUsers } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { AdminUser } from '@/api/types';
import { AdminToolbar, Chip, QueryState, useAdminTimeZone } from '@/components/admin/AdminUi';
import { formatBytes, formatCount, formatDateTime } from '@/components/admin/format';
import { UserCreateDialog, UserEditDialog } from '@/components/admin/UserDialogs';
import { Avatar, Button, ConfirmDialog, Icon, IconButton } from '@/components/common';
import { useMe } from '@/components/compose/useMe';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';

export function AdminUsers() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const me = useMe();
  const users = useQuery({ queryKey: adminKeys.users(), queryFn: ({ signal }) => adminListUsers(signal), staleTime: staleTimes.admin });
  const domains = useQuery({ queryKey: adminKeys.domains(), queryFn: ({ signal }) => adminListDomains(signal), staleTime: staleTimes.admin });
  const [creating, setCreating] = useState(false);
  const [editing, setEditing] = useState<AdminUser | null>(null);
  const [deleting, setDeleting] = useState<AdminUser | null>(null);

  const del = useMutation({
    mutationFn: (u: AdminUser) => adminDeleteUser(u.id),
    onSuccess: (_v, u) => {
      void qc.invalidateQueries({ queryKey: adminKeys.all() });
      toast.push({ message: t('admin.users.deleted', { email: u.email }) });
      setDeleting(null);
    },
    onError: (e) => {
      toast.error(t('admin.common.deleteFailed', { reason: errorMessage(e) }));
      setDeleting(null);
    },
  });

  const myId = me.data?.id;

  return (
    <>
      <AdminToolbar
        actions={
          <Button icon="person_add" onClick={() => setCreating(true)} disabled={!domains.data}>
            {t('admin.users.create')}
          </Button>
        }
      />
      <QueryState query={users} empty={t('admin.users.empty')}>
        {(list) => (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th>{t('admin.users.name')}</th>
                  <th>{t('admin.users.email')}</th>
                  <th>{t('admin.users.admin')}</th>
                  <th>{t('admin.users.status')}</th>
                  <th>{t('admin.users.lastLogin')}</th>
                  <th className="num">{t('admin.users.messages')}</th>
                  <th className="num">{t('admin.users.storage')}</th>
                  <th className="text-right">{t('admin.common.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {list.map((u) => {
                  const self = u.id === myId;
                  return (
                    <tr key={u.id}>
                      <td>
                        <span className="flex min-w-0 items-center gap-3">
                          <Avatar name={u.display_name} email={u.email} size={28} decorative />
                          <span className="truncate font-medium">{u.display_name || '—'}</span>
                          {self && <Chip tone="info">{t('admin.users.you')}</Chip>}
                        </span>
                      </td>
                      <td className="whitespace-nowrap">{u.email}</td>
                      <td>
                        {u.is_admin ? (
                          <Icon name="check" label={t('admin.common.yes')} className="text-primary" />
                        ) : (
                          <span className="text-on-surface-variant">{t('admin.common.none')}</span>
                        )}
                      </td>
                      <td>
                        <Chip tone={u.disabled ? 'error' : 'success'}>{u.disabled ? t('admin.users.disabled') : t('admin.users.active')}</Chip>
                      </td>
                      <td className="whitespace-nowrap text-on-surface-variant">
                        {u.last_login_at ? formatDateTime(u.last_login_at, tz) : t('admin.common.never')}
                      </td>
                      <td className="num">{formatCount(u.message_count)}</td>
                      <td className="num whitespace-nowrap">{formatBytes(u.storage_bytes)}</td>
                      <td className="text-right whitespace-nowrap">
                        <IconButton icon="edit" size="sm" label={`${t('admin.common.edit')} ${u.email}`} onClick={() => setEditing(u)} />
                        <IconButton
                          icon="delete"
                          size="sm"
                          label={self ? t('admin.users.cannotDeleteSelf') : `${t('admin.common.delete')} ${u.email}`}
                          disabled={self}
                          onClick={() => setDeleting(u)}
                        />
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>

      {creating && domains.data && <UserCreateDialog domains={domains.data} onClose={() => setCreating(false)} />}
      {editing && <UserEditDialog user={editing} isSelf={editing.id === myId} onClose={() => setEditing(null)} />}
      <ConfirmDialog
        open={deleting !== null}
        onOpenChange={(o) => !o && !del.isPending && setDeleting(null)}
        title={t('admin.users.deleteTitle', { email: deleting?.email ?? '' })}
        message={t('admin.users.deleteMessage')}
        confirmLabel={t('admin.common.delete')}
        danger
        busy={del.isPending}
        onConfirm={() => deleting && del.mutate(deleting)}
      />
    </>
  );
}
