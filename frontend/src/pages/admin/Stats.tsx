/**
 * Admin → 统计 [WP-F]: cards for users, messages, attachment storage (backend / delivery /
 * blob count / bytes), the job queue (pending, of which periodic / dead), the last 24 h (sent /
 * received / failed) and the last webhook / poll; warning banners while Resend's quota is
 * exhausted and when the inbound poller detected a gap (`poll_gap`, DESIGN B7); and "立即同步收件"
 * (POST /api/admin/sync → enqueues poll.receiving). Refreshes every 30 s.
 *
 * Contract: `export function AdminStats()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import type { ReactNode } from 'react';
import { errorMessage } from '@/api/client';
import { adminGetStats, adminKeys, adminSync } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import type { AdminStats as AdminStatsData } from '@/api/types';
import { AdminToolbar, QueryState, useAdminTimeZone } from '@/components/admin/AdminUi';
import { formatBytes, formatCount, formatDateTime } from '@/components/admin/format';
import { Button, cx, Icon, IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';

export const STATS_REFRESH_MS = 30_000;

function StatCard({ icon, title, children, tone }: { icon: string; title: string; children: ReactNode; tone?: 'error' }) {
  return (
    <section className={cx('azm-card flex flex-col gap-2', tone === 'error' && 'border-error/40')} aria-label={title}>
      <h2 className="m-0 flex items-center gap-2 text-sm font-medium text-on-surface-variant">
        <Icon name={icon} size={20} className={tone === 'error' ? 'text-error' : 'text-primary'} />
        {title}
      </h2>
      {children}
    </section>
  );
}

function Big({ children, tone }: { children: ReactNode; tone?: 'error' }) {
  return <p className={cx('m-0 text-[28px] leading-9 font-normal tabular-nums', tone === 'error' ? 'text-error' : 'text-on-surface')}>{children}</p>;
}

function Pair({ label, value, tone }: { label: string; value: ReactNode; tone?: 'error' }) {
  return (
    <div className="flex flex-col">
      <span className="text-xs text-on-surface-variant">{label}</span>
      <span className={cx('text-xl tabular-nums', tone === 'error' ? 'text-error' : 'text-on-surface')}>{value}</span>
    </div>
  );
}

function StatsGrid({ s, tz }: { s: AdminStatsData; tz: string }) {
  return (
    <div className="flex flex-col gap-4">
      {s.quota_blocked && (
        <div className="azm-banner tone-warning" role="alert">
          <Icon name="warning" size={20} />
          <span>{t('admin.stats.quotaBlocked')}</span>
        </div>
      )}
      {s.poll_gap && (
        <div className="azm-banner tone-warning" role="alert" data-testid="poll-gap">
          <Icon name="sync_problem" size={20} />
          <div className="flex min-w-0 flex-col gap-0.5">
            <span className="font-medium">{t('admin.stats.pollGapTitle')}</span>
            <span>{t('admin.stats.pollGapHint', { time: formatDateTime(s.poll_gap.detected_at, tz, { seconds: true }) })}</span>
            {s.poll_gap.detail && <span className="break-words text-xs opacity-80">{s.poll_gap.detail}</span>}
          </div>
        </div>
      )}
      <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">
        <StatCard icon="group" title={t('admin.stats.users')}>
          <Big>{formatCount(s.users)}</Big>
        </StatCard>
        <StatCard icon="mail" title={t('admin.stats.messages')}>
          <Big>{formatCount(s.messages)}</Big>
          <p className="m-0 text-xs text-on-surface-variant">{t('admin.stats.mailboxBytes', { size: formatBytes(s.storage_bytes) })}</p>
        </StatCard>
        <StatCard icon="cloud" title={t('admin.stats.storage')}>
          <Big>{t(`admin.stats.backends.${s.storage.backend}` as const)}</Big>
          <p className="m-0 text-xs text-on-surface-variant">
            {t(`admin.stats.deliveries.${s.storage.delivery}` as const)} ·{' '}
            {t('admin.stats.blobs', { count: formatCount(s.storage.blob_count), size: formatBytes(s.storage.blob_bytes) })}
          </p>
        </StatCard>
        <StatCard icon="pending_actions" title={t('admin.stats.queue')} tone={s.queue.dead > 0 ? 'error' : undefined}>
          <div className="flex gap-8">
            <Pair
              label={t('admin.stats.pending')}
              value={
                <span data-testid="queue-pending">
                  {formatCount(s.queue.pending)}
                  {/* "待处理 N（其中周期任务 M）": the recurring jobs are always queued (older backends omit it). */}
                  {typeof s.queue.periodic === 'number' && (
                    <span className="text-sm text-on-surface-variant">{t('admin.stats.pendingPeriodic', { count: formatCount(s.queue.periodic) })}</span>
                  )}
                </span>
              }
            />
            <Pair label={t('admin.stats.dead')} value={formatCount(s.queue.dead)} tone={s.queue.dead > 0 ? 'error' : undefined} />
          </div>
        </StatCard>
        <StatCard icon="monitoring" title={t('admin.stats.last24h')} tone={s.failed_24h > 0 ? 'error' : undefined}>
          <div className="flex gap-8">
            <Pair label={t('admin.stats.sent')} value={formatCount(s.sent_24h)} />
            <Pair label={t('admin.stats.received')} value={formatCount(s.received_24h)} />
            <Pair label={t('admin.stats.failed')} value={formatCount(s.failed_24h)} tone={s.failed_24h > 0 ? 'error' : undefined} />
          </div>
        </StatCard>
        <StatCard icon="sync" title={t('admin.stats.syncStatus')}>
          <div className="flex flex-col gap-2 text-sm">
            <div className="flex justify-between gap-4">
              <span className="text-on-surface-variant">{t('admin.stats.lastWebhook')}</span>
              <span className="tabular-nums">{s.last_webhook_at ? formatDateTime(s.last_webhook_at, tz, { seconds: true }) : t('admin.common.never')}</span>
            </div>
            <div className="flex justify-between gap-4">
              <span className="text-on-surface-variant">{t('admin.stats.lastPoll')}</span>
              <span className="tabular-nums">{s.last_poll_at ? formatDateTime(s.last_poll_at, tz, { seconds: true }) : t('admin.common.never')}</span>
            </div>
          </div>
        </StatCard>
      </div>
    </div>
  );
}

export function AdminStats() {
  const qc = useQueryClient();
  const tz = useAdminTimeZone();
  const query = useQuery({
    queryKey: adminKeys.stats(),
    queryFn: ({ signal }) => adminGetStats(signal),
    staleTime: staleTimes.admin,
    refetchInterval: STATS_REFRESH_MS,
  });
  const sync = useMutation({
    mutationFn: () => adminSync(),
    onSuccess: (r) => {
      toast.push({ message: t('admin.stats.syncStarted', { id: r.job_id }) });
      setTimeout(() => void qc.invalidateQueries({ queryKey: adminKeys.stats() }), 3000);
    },
    onError: (e) => toast.error(errorMessage(e)),
  });
  return (
    <>
      <AdminToolbar
        actions={
          <>
            <IconButton icon="refresh" label={t('admin.common.refresh')} onClick={() => void query.refetch()} disabled={query.isFetching} />
            <Button icon="cloud_sync" variant="tonal" loading={sync.isPending} onClick={() => sync.mutate()}>
              {t('admin.stats.sync')}
            </Button>
          </>
        }
      >
        {query.dataUpdatedAt > 0 && (
          <span className="text-xs text-on-surface-variant">{t('admin.stats.updatedAt', { time: formatDateTime(query.dataUpdatedAt, tz, { seconds: true }) })}</span>
        )}
      </AdminToolbar>
      <QueryState query={query}>{(s) => <StatsGrid s={s} tz={tz} />}</QueryState>
    </>
  );
}
