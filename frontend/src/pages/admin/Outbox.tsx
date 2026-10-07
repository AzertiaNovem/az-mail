/**
 * Admin → 发件队列 [WP-F], three views:
 * - 发件: outbound rows filtered by failed / queued / sending; 重试 on a failed row re-sends it
 *   as a NEW outbound (the old row stays failed), so the whole `['admin','outbox']` prefix is
 *   invalidated;
 * - 收件异常: unroutable / failed inbound mail (envelope metadata only);
 * - 失败任务: dead jobs with 重试.
 *
 * Contract: `export function AdminOutbox()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { errorMessage } from '@/api/client';
import {
  adminKeys,
  adminListInbound,
  adminListJobs,
  adminListOutbox,
  adminRetryJob,
  adminRetryOutbox,
  type AdminInboundFilter,
  type AdminOutboxFilter,
} from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { InboundRow } from '@/api/types';
import { AdminToolbar, Chip, OutboundStatusChip, QueryState, useAdminTimeZone } from '@/components/admin/AdminUi';
import { formatBytes, formatDateTime } from '@/components/admin/format';
import { Button, Icon, IconButton } from '@/components/common';
import { t, zh } from '@/i18n/zh';
import { toast } from '@/stores/toast';

type Section = 'outbound' | 'inbound' | 'jobs';
const SECTIONS: Section[] = ['outbound', 'inbound', 'jobs'];

function Segmented<V extends string>({ value, options, onChange, label }: { value: V; options: { value: V; label: string }[]; onChange: (v: V) => void; label: string }) {
  return (
    <div className="azm-segmented" role="group" aria-label={label}>
      {options.map((o) => (
        <button key={o.value} type="button" aria-pressed={o.value === value} onClick={() => onChange(o.value)}>
          {o.value === value && <Icon name="check" size={16} />}
          {o.label}
        </button>
      ))}
    </div>
  );
}

function OutboundView() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const [status, setStatus] = useState<AdminOutboxFilter>('failed');
  const query = useQuery({ queryKey: adminKeys.outbox(status), queryFn: ({ signal }) => adminListOutbox(status, signal), staleTime: staleTimes.admin });
  const retry = useMutation({
    mutationFn: (id: number) => adminRetryOutbox(id),
    onSuccess: (row) => {
      // The retry creates a NEW row: refresh every status filter (the ['admin','outbox'] prefix).
      void qc.invalidateQueries({ queryKey: ['admin', 'outbox'] });
      void qc.invalidateQueries({ queryKey: adminKeys.stats() });
      toast.push({ message: t('admin.outbox.retried', { id: row.id }) });
    },
    onError: (e) => toast.error(errorMessage(e)),
  });
  const filters = zh.admin.outbox.filters;
  return (
    <>
      <AdminToolbar
        actions={<IconButton icon="refresh" label={t('admin.common.refresh')} onClick={() => void query.refetch()} disabled={query.isFetching} />}
      >
        <Segmented
          label={t('admin.outbox.status')}
          value={status}
          onChange={setStatus}
          options={[
            { value: 'failed', label: filters.failed },
            { value: 'queued', label: filters.queued },
            { value: 'sending', label: filters.sending },
          ]}
        />
      </AdminToolbar>
      <QueryState query={query} empty={t('admin.outbox.empty')}>
        {(rows) => (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th>{t('admin.outbox.id')}</th>
                  <th>{t('admin.outbox.from')}</th>
                  <th>{t('admin.outbox.status')}</th>
                  <th>{t('admin.outbox.detail')}</th>
                  <th>{t('admin.outbox.scheduled')}</th>
                  <th className="num">{t('admin.outbox.size')}</th>
                  <th>{t('admin.outbox.updated')}</th>
                  <th className="text-right">{t('admin.common.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((r) => (
                  <tr key={r.id}>
                    <td className="azm-mono whitespace-nowrap" title={r.uuid}>
                      #{r.id}
                    </td>
                    <td className="whitespace-nowrap">
                      <span className="block">{r.from_email}</span>
                      {r.sender_email !== r.from_email && (
                        <span className="block text-xs text-on-surface-variant">
                          {t('admin.outbox.sender')}：{r.sender_email}
                        </span>
                      )}
                    </td>
                    <td>
                      <OutboundStatusChip status={r.status} />
                    </td>
                    <td className="min-w-[160px] max-w-[260px] text-xs text-on-surface-variant">
                      <span className="line-clamp-2" title={[r.error_name, r.status_detail].filter(Boolean).join(' · ')}>
                        {r.status_detail ?? r.error_name ?? t('admin.common.none')}
                      </span>
                    </td>
                    <td className="whitespace-nowrap text-xs">
                      {r.scheduled_at ? (
                        <>
                          {formatDateTime(r.scheduled_at, tz)}
                          {r.scheduled_via && <span className="block text-on-surface-variant">{t(`admin.outbox.via.${r.scheduled_via}` as const)}</span>}
                        </>
                      ) : (
                        t('admin.common.none')
                      )}
                    </td>
                    <td className="num whitespace-nowrap">{formatBytes(r.total_bytes)}</td>
                    <td className="min-w-[88px] text-on-surface-variant">{formatDateTime(r.updated_at, tz)}</td>
                    <td className="text-right">
                      {r.status === 'failed' && (
                        <Button
                          variant="text"
                          size="sm"
                          loading={retry.isPending && retry.variables === r.id}
                          onClick={() => retry.mutate(r.id)}
                        >
                          {t('admin.outbox.retry')}
                        </Button>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>
    </>
  );
}

const INBOUND_TONES: Record<InboundRow['state'], 'warning' | 'error' | 'info' | 'success'> = {
  pending: 'info',
  delivered: 'success',
  unroutable: 'warning',
  failed: 'error',
};

function InboundView() {
  const tz = useAdminTimeZone();
  const [state, setState] = useState<AdminInboundFilter>('unroutable');
  const query = useQuery({ queryKey: adminKeys.inbound(state), queryFn: ({ signal }) => adminListInbound(state, signal), staleTime: staleTimes.admin });
  const labels = zh.admin.outbox.inbound;
  return (
    <>
      <AdminToolbar
        actions={<IconButton icon="refresh" label={t('admin.common.refresh')} onClick={() => void query.refetch()} disabled={query.isFetching} />}
      >
        <Segmented
          label={labels.state}
          value={state}
          onChange={setState}
          options={[
            { value: 'unroutable', label: zh.admin.outbox.filters.unroutable },
            { value: 'failed', label: zh.admin.outbox.filters.inboundFailed },
          ]}
        />
      </AdminToolbar>
      <QueryState query={query} empty={labels.empty}>
        {(rows) => (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th>{labels.received}</th>
                  <th>{labels.from}</th>
                  <th>{labels.subject}</th>
                  <th>{labels.recipients}</th>
                  <th>{labels.state}</th>
                  <th>{labels.source}</th>
                  <th>{labels.error}</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((r) => (
                  <tr key={r.id}>
                    <td className="whitespace-nowrap">{formatDateTime(r.received_at ?? r.created_at, tz)}</td>
                    <td className="break-all">{r.from_email ?? t('admin.common.none')}</td>
                    <td className="max-w-[240px] truncate" title={r.subject ?? ''}>
                      {r.subject || t('common.noSubject')}
                    </td>
                    <td className="max-w-[220px] text-xs break-all">{r.recipients.join(', ') || t('admin.common.none')}</td>
                    <td>
                      <Chip tone={INBOUND_TONES[r.state]}>{labels.states[r.state]}</Chip>
                    </td>
                    <td className="whitespace-nowrap">{labels.sources[r.source] ?? r.source}</td>
                    <td className="max-w-[260px] text-xs text-on-surface-variant">
                      <span className="line-clamp-2" title={r.error ?? ''}>
                        {r.error ?? t('admin.common.none')}
                      </span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>
    </>
  );
}

function JobsView() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const query = useQuery({ queryKey: adminKeys.jobs('dead'), queryFn: ({ signal }) => adminListJobs('dead', signal), staleTime: staleTimes.admin });
  const retry = useMutation({
    mutationFn: (id: number) => adminRetryJob(id),
    onSuccess: (job) => {
      void qc.invalidateQueries({ queryKey: ['admin', 'jobs'] });
      void qc.invalidateQueries({ queryKey: adminKeys.stats() });
      toast.push({ message: t('admin.outbox.jobs.retried', { id: job.id }) });
    },
    onError: (e) => toast.error(errorMessage(e)),
  });
  const labels = zh.admin.outbox.jobs;
  return (
    <>
      <AdminToolbar
        actions={<IconButton icon="refresh" label={t('admin.common.refresh')} onClick={() => void query.refetch()} disabled={query.isFetching} />}
      />
      <QueryState query={query} empty={labels.empty}>
        {(rows) => (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th>{t('admin.outbox.id')}</th>
                  <th>{labels.kind}</th>
                  <th>{labels.lane}</th>
                  <th className="num">{labels.attempts}</th>
                  <th>{labels.error}</th>
                  <th>{t('admin.outbox.updated')}</th>
                  <th className="text-right">{t('admin.common.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((j) => (
                  <tr key={j.id}>
                    <td className="azm-mono">#{j.id}</td>
                    <td className="azm-mono whitespace-nowrap">{j.kind}</td>
                    <td className="whitespace-nowrap">{j.lane}</td>
                    <td className="num">
                      {j.attempts}/{j.max_attempts}
                    </td>
                    <td className="max-w-[320px] text-xs text-on-surface-variant">
                      <span className="line-clamp-2" title={j.last_error ?? ''}>
                        {j.last_error ?? t('admin.common.none')}
                      </span>
                    </td>
                    <td className="min-w-[88px] text-on-surface-variant">{formatDateTime(j.updated_at, tz)}</td>
                    <td className="text-right">
                      <Button
                        variant="text"
                        size="sm"
                        loading={retry.isPending && retry.variables === j.id}
                        onClick={() => retry.mutate(j.id)}
                      >
                        {t('admin.outbox.retry')}
                      </Button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </QueryState>
    </>
  );
}

export function AdminOutbox() {
  const [section, setSection] = useState<Section>('outbound');
  return (
    <>
      <div role="tablist" aria-label={t('admin.outbox.sectionsLabel')} className="mb-4 flex gap-1 border-b border-divider">
        {SECTIONS.map((s) => (
          <button
            key={s}
            type="button"
            role="tab"
            aria-selected={s === section}
            className="azm-tab h-10"
            onClick={() => setSection(s)}
          >
            {t(`admin.outbox.sections.${s}` as const)}
          </button>
        ))}
      </div>
      <div role="tabpanel">{section === 'outbound' ? <OutboundView /> : section === 'inbound' ? <InboundView /> : <JobsView />}</div>
    </>
  );
}
