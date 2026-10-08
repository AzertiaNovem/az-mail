/**
 * Admin → 域名 [WP-F]: domain list with add (validated name), delete (409 domain_in_use while
 * addresses use it) and the "查看 DNS 状态" dialog (Resend verification + DNS records).
 *
 * Contract: `export function AdminDomains()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { adminCreateDomain, adminDeleteDomain, adminKeys, adminListDomains } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { DomainRow } from '@/api/types';
import { AdminToolbar, Chip, QueryState, useAdminTimeZone } from '@/components/admin/AdminUi';
import { DomainStatusDialog } from '@/components/admin/DomainStatusDialog';
import { formatDateTime } from '@/components/admin/format';
import { validateDomainName } from '@/components/admin/validation';
import { Button, ConfirmDialog, IconButton, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';

export function AdminDomains() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const domains = useQuery({ queryKey: adminKeys.domains(), queryFn: ({ signal }) => adminListDomains(signal), staleTime: staleTimes.admin });
  const [name, setName] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [viewing, setViewing] = useState<DomainRow | null>(null);
  const [deleting, setDeleting] = useState<DomainRow | null>(null);

  const add = useMutation({
    mutationFn: (n: string) => adminCreateDomain({ name: n }),
    onSuccess: (d) => {
      void qc.invalidateQueries({ queryKey: adminKeys.domains() });
      toast.push({ message: t('admin.domains.added', { name: d.name }) });
      setName('');
    },
    onError: (e) => setError(errorMessage(e)),
  });

  const del = useMutation({
    mutationFn: (d: DomainRow) => adminDeleteDomain(d.id),
    onSuccess: (_v, d) => {
      void qc.invalidateQueries({ queryKey: adminKeys.domains() });
      toast.push({ message: t('admin.domains.deleted', { name: d.name }) });
      setDeleting(null);
    },
    onError: (e) => {
      toast.error(isApiError(e, 'domain_in_use') ? t('errors.domain_in_use') : t('admin.common.deleteFailed', { reason: errorMessage(e) }));
      setDeleting(null);
    },
  });

  const submit = (e: FormEvent) => {
    e.preventDefault();
    const err = validateDomainName(name);
    setError(err);
    if (!err) add.mutate(name.trim().toLowerCase());
  };

  return (
    <>
      <AdminToolbar>
        <form className="flex w-full max-w-md items-start gap-2" onSubmit={submit} noValidate>
          <TextField
            aria-label={t('admin.domains.name')}
            placeholder={t('admin.domains.namePlaceholder')}
            containerClassName="flex-1"
            value={name}
            autoCapitalize="off"
            spellCheck={false}
            error={error ?? undefined}
            onChange={(e) => {
              setName(e.target.value);
              setError(null);
            }}
          />
          <Button type="submit" icon="add" loading={add.isPending} className="h-10">
            {t('admin.domains.add')}
          </Button>
        </form>
      </AdminToolbar>
      <QueryState query={domains} empty={t('admin.domains.empty')}>
        {(list) => (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th>{t('admin.domains.name')}</th>
                  <th>{t('admin.domains.receiving')}</th>
                  <th>{t('admin.domains.createdAt')}</th>
                  <th className="text-right">{t('admin.common.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {list.map((d) => (
                  <tr key={d.id}>
                    <td className="font-medium whitespace-nowrap">{d.name}</td>
                    <td>
                      <Chip tone={d.receiving_enabled ? 'success' : 'neutral'}>
                        {d.receiving_enabled ? t('admin.common.yes') : t('admin.common.no')}
                      </Chip>
                    </td>
                    <td className="whitespace-nowrap text-on-surface-variant">{formatDateTime(d.created_at, tz)}</td>
                    <td className="text-right whitespace-nowrap">
                      <Button variant="text" size="sm" icon="dns" onClick={() => setViewing(d)}>
                        {t('admin.domains.status')}
                      </Button>
                      <IconButton icon="delete" size="sm" label={`${t('admin.common.delete')} ${d.name}`} onClick={() => setDeleting(d)} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>
      {viewing && <DomainStatusDialog domain={viewing} onClose={() => setViewing(null)} />}
      <ConfirmDialog
        open={deleting !== null}
        onOpenChange={(o) => !o && !del.isPending && setDeleting(null)}
        title={t('admin.domains.deleteTitle', { name: deleting?.name ?? '' })}
        message={t('admin.domains.deleteMessage')}
        confirmLabel={t('admin.common.delete')}
        danger
        busy={del.isPending}
        onConfirm={() => deleting && del.mutate(deleting)}
      />
    </>
  );
}
