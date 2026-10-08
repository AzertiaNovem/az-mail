/**
 * Admin → 别名 [WP-F]: alias table (address, members with 可代发, 共享已发送) with create / edit
 * (AliasDialog) and delete. An alias that sent mail cannot be deleted (409 alias_in_use) —
 * removing its members disables it instead.
 *
 * Contract: `export function AdminAliases()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { adminDeleteAlias, adminKeys, adminListAliases, adminListDomains, adminListUsers } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { AdminAlias } from '@/api/types';
import { AdminToolbar, Chip, EmailAddress, QueryState, useAdminTimeZone } from '@/components/admin/AdminUi';
import { AliasDialog } from '@/components/admin/AliasDialog';
import { formatDateTime } from '@/components/admin/format';
import { Button, ConfirmDialog, IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';

export function AdminAliases() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const aliases = useQuery({ queryKey: adminKeys.aliases(), queryFn: ({ signal }) => adminListAliases(signal), staleTime: staleTimes.admin });
  const users = useQuery({ queryKey: adminKeys.users(), queryFn: ({ signal }) => adminListUsers(signal), staleTime: staleTimes.admin });
  const domains = useQuery({ queryKey: adminKeys.domains(), queryFn: ({ signal }) => adminListDomains(signal), staleTime: staleTimes.admin });
  const [editing, setEditing] = useState<AdminAlias | 'new' | null>(null);
  const [deleting, setDeleting] = useState<AdminAlias | null>(null);

  const del = useMutation({
    mutationFn: (a: AdminAlias) => adminDeleteAlias(a.id),
    onSuccess: (_v, a) => {
      void qc.invalidateQueries({ queryKey: adminKeys.all() });
      toast.push({ message: t('admin.aliases.deleted', { email: a.email }) });
      setDeleting(null);
    },
    onError: (e) => {
      toast.error(isApiError(e, 'alias_in_use') ? t('errors.alias_in_use') : t('admin.common.deleteFailed', { reason: errorMessage(e) }), 10_000);
      setDeleting(null);
    },
  });

  const ready = !!users.data && !!domains.data;

  return (
    <>
      <AdminToolbar
        actions={
          <Button icon="group_add" onClick={() => setEditing('new')} disabled={!ready}>
            {t('admin.aliases.create')}
          </Button>
        }
      />
      <QueryState query={aliases} empty={t('admin.aliases.empty')}>
        {(list) => (
          <div className="azm-table-wrap">
            <table className="azm-table min-w-[640px]">
              <thead>
                <tr>
                  <th className="min-w-[12rem]">{t('admin.aliases.email')}</th>
                  <th className="min-w-[10rem]">{t('admin.aliases.members')}</th>
                  <th>{t('admin.aliases.shareSent')}</th>
                  <th>{t('admin.domains.createdAt')}</th>
                  <th className="text-right">{t('admin.common.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {list.map((a) => (
                  <tr key={a.id}>
                    <td>
                      <EmailAddress email={a.email} className="block font-medium" />
                      {a.display_name && <span className="block truncate text-xs text-on-surface-variant">{a.display_name}</span>}
                    </td>
                    <td className="py-2">
                      {a.members.length === 0 ? (
                        <Chip tone="warning">{t('admin.aliases.noMembers')}</Chip>
                      ) : (
                        <span className="flex flex-wrap gap-1">
                          {a.members.map((m) => (
                            <Chip key={m.user_id} tone={m.can_send_as ? 'info' : 'neutral'} title={m.email}>
                              {m.display_name || m.email}
                              {m.can_send_as && <span className="opacity-80">· {t('admin.aliases.canSendAs')}</span>}
                            </Chip>
                          ))}
                        </span>
                      )}
                    </td>
                    <td className="whitespace-nowrap">{a.share_sent ? t('admin.common.yes') : t('admin.common.no')}</td>
                    <td className="whitespace-nowrap text-on-surface-variant">{formatDateTime(a.created_at, tz)}</td>
                    <td className="text-right whitespace-nowrap">
                      <IconButton
                        icon="edit"
                        size="sm"
                        label={`${t('admin.common.edit')} ${a.email}`}
                        disabled={!ready}
                        onClick={() => setEditing(a)}
                      />
                      <IconButton icon="delete" size="sm" label={`${t('admin.common.delete')} ${a.email}`} onClick={() => setDeleting(a)} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>

      {editing !== null && ready && (
        <AliasDialog
          alias={editing === 'new' ? null : editing}
          users={users.data ?? []}
          domains={domains.data ?? []}
          onClose={() => setEditing(null)}
        />
      )}
      <ConfirmDialog
        open={deleting !== null}
        onOpenChange={(o) => !o && !del.isPending && setDeleting(null)}
        title={t('admin.aliases.deleteTitle', { email: deleting?.email ?? '' })}
        message={t('admin.aliases.deleteMessage')}
        confirmLabel={t('admin.common.delete')}
        danger
        busy={del.isPending}
        onConfirm={() => deleting && del.mutate(deleting)}
      />
    </>
  );
}
