/**
 * "查看 DNS 状态" dialog [WP-F]: GET /api/admin/domains/:id/status (proxied from Resend) →
 * verification state and the DNS records to configure, each value with a copy button.
 */
import { useQuery } from '@tanstack/react-query';
import { adminGetDomainStatus, adminKeys } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { DomainRow } from '@/api/types';
import { Button, Dialog } from '@/components/common';
import { zh, t } from '@/i18n/zh';
import { Chip, CopyButton, QueryState, type ChipTone } from './AdminUi';

const STATE_TONES: Record<string, ChipTone> = {
  verified: 'success',
  pending: 'info',
  not_started: 'neutral',
  failed: 'error',
  temporary_failure: 'warning',
};

export function DomainStateChip({ status }: { status: string }) {
  const label = (zh.admin.domains.states as Record<string, string>)[status] ?? status;
  return <Chip tone={STATE_TONES[status] ?? 'neutral'}>{label}</Chip>;
}

export function DomainStatusDialog({ domain, onClose }: { domain: DomainRow; onClose: () => void }) {
  const query = useQuery({
    queryKey: adminKeys.domainStatus(domain.id),
    queryFn: ({ signal }) => adminGetDomainStatus(domain.id, signal),
    staleTime: staleTimes.admin,
    retry: false,
  });
  return (
    <Dialog
      open
      onOpenChange={(o) => !o && onClose()}
      title={t('admin.domains.statusTitle', { name: domain.name })}
      size="xl"
      footer={
        <>
          <Button variant="text" icon="refresh" onClick={() => void query.refetch()} disabled={query.isFetching}>
            {t('admin.common.refresh')}
          </Button>
          <Button onClick={onClose}>{t('actions.close')}</Button>
        </>
      }
    >
      <QueryState query={query}>
        {(status) =>
          status.resend === null ? (
            <div className="azm-banner tone-warning">{t('admin.domains.notInResend')}</div>
          ) : (
            <div className="flex flex-col gap-4 pb-2">
              <dl className="m-0 flex flex-wrap items-center gap-x-8 gap-y-2 text-sm">
                <div className="flex items-center gap-2">
                  <dt className="text-on-surface-variant">{t('admin.domains.resendStatus')}</dt>
                  <dd className="m-0">
                    <DomainStateChip status={status.resend.status} />
                  </dd>
                </div>
                {status.resend.region && (
                  <div className="flex items-center gap-2">
                    <dt className="text-on-surface-variant">{t('admin.domains.region')}</dt>
                    <dd className="m-0">{status.resend.region}</dd>
                  </div>
                )}
              </dl>
              <h3 className="m-0 text-sm font-medium">{t('admin.domains.records')}</h3>
              {status.resend.records.length === 0 ? (
                <p className="m-0 text-sm text-on-surface-variant">{t('admin.domains.noRecords')}</p>
              ) : (
                <div className="azm-table-wrap">
                  <table className="azm-table">
                    <thead>
                      <tr>
                        <th>{t('admin.domains.recordType')}</th>
                        <th>{t('admin.domains.recordName')}</th>
                        <th>{t('admin.domains.recordValue')}</th>
                        <th className="num">{t('admin.domains.recordPriority')}</th>
                        <th>{t('admin.domains.recordTtl')}</th>
                        <th>{t('admin.domains.recordStatus')}</th>
                      </tr>
                    </thead>
                    <tbody>
                      {status.resend.records.map((r, i) => (
                        <tr key={`${r.type}:${r.name}:${i}`}>
                          <td className="whitespace-nowrap">
                            <span className="font-medium">{r.type}</span>
                            <span className="ml-1 text-xs text-on-surface-variant">{r.record}</span>
                          </td>
                          <td className="azm-mono max-w-[180px] break-all">{r.name}</td>
                          <td className="max-w-[320px]">
                            <div className="flex items-center gap-1">
                              <code className="azm-mono min-w-0 flex-1 break-all">{r.value}</code>
                              <CopyButton text={r.value} label={t('admin.domains.copyValue')} />
                            </div>
                          </td>
                          <td className="num">{r.priority ?? ''}</td>
                          <td className="whitespace-nowrap">{r.ttl}</td>
                          <td>
                            <DomainStateChip status={r.status} />
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </div>
          )
        }
      </QueryState>
    </Dialog>
  );
}
