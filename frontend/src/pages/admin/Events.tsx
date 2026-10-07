/**
 * Admin → Webhook 事件 [WP-F]: received Resend webhooks (metadata only — admins never see mail
 * bodies) with a type filter and cursor paging (a cursor stack gives 上一页 / 下一页).
 *
 * Contract: `export function AdminEvents()`, no props, rendered inside AdminLayout's `<Outlet/>`.
 */
import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { useState } from 'react';
import { adminKeys, adminListEvents } from '@/api/admin';
import { staleTimes } from '@/api/queryKeys';
import { AdminToolbar, Chip, QueryState, useAdminTimeZone, type ChipTone } from '@/components/admin/AdminUi';
import { formatDateTime } from '@/components/admin/format';
import { Button, IconButton, Select } from '@/components/common';
import { t, zh } from '@/i18n/zh';

export const WEBHOOK_EVENT_TYPES = [
  'email.sent',
  'email.delivered',
  'email.delivery_delayed',
  'email.bounced',
  'email.complained',
  'email.failed',
  'email.suppressed',
  'email.scheduled',
  'email.opened',
  'email.clicked',
  'email.received',
] as const;

export function resultChip(result: string | null): { tone: ChipTone; label: string } {
  const labels = zh.admin.events.results;
  if (result === null) return { tone: 'warning', label: labels.pending };
  if (result.startsWith('error')) return { tone: 'error', label: labels.error };
  switch (result) {
    case 'applied':
      return { tone: 'success', label: labels.applied };
    case 'enqueued':
      return { tone: 'info', label: labels.enqueued };
    case 'ignored_unknown':
      return { tone: 'neutral', label: labels.ignored_unknown };
    case 'duplicate':
      return { tone: 'neutral', label: labels.duplicate };
    default:
      return { tone: 'neutral', label: result };
  }
}

export function AdminEvents() {
  const tz = useAdminTimeZone();
  const [type, setType] = useState('');
  // cursors[i] = cursor of page i (null for the first page).
  const [cursors, setCursors] = useState<(string | null)[]>([null]);
  const cursor = cursors[cursors.length - 1] ?? null;
  const query = useQuery({
    queryKey: adminKeys.events(type || null, cursor),
    queryFn: ({ signal }) => adminListEvents({ type: type || undefined, cursor }, signal),
    staleTime: staleTimes.admin,
    placeholderData: keepPreviousData,
  });
  const nextCursor = query.data?.next_cursor ?? null;

  return (
    <>
      <AdminToolbar
        actions={
          <IconButton icon="refresh" label={t('admin.common.refresh')} onClick={() => void query.refetch()} disabled={query.isFetching} />
        }
      >
        <Select
          aria-label={t('admin.events.type')}
          containerClassName="w-64"
          options={[{ value: '', label: t('admin.events.allTypes') }, ...WEBHOOK_EVENT_TYPES.map((v) => ({ value: v, label: v }))]}
          value={type}
          onValueChange={(v) => {
            setType(v);
            setCursors([null]);
          }}
        />
      </AdminToolbar>
      <QueryState query={query}>
        {(page) =>
          page.items.length === 0 ? (
            <p className="py-12 text-center text-sm text-on-surface-variant">{t('admin.events.empty')}</p>
          ) : (
            <div className="azm-table-wrap">
              <table className="azm-table">
                <thead>
                  <tr>
                    <th>{t('admin.events.received')}</th>
                    <th>{t('admin.events.type')}</th>
                    <th>{t('admin.events.email')}</th>
                    <th>{t('admin.events.svix')}</th>
                    <th>{t('admin.events.processed')}</th>
                    <th>{t('admin.events.result')}</th>
                  </tr>
                </thead>
                <tbody>
                  {page.items.map((ev) => {
                    const chip = resultChip(ev.result);
                    return (
                      <tr key={ev.id}>
                        <td className="whitespace-nowrap">{formatDateTime(ev.received_at, tz, { seconds: true })}</td>
                        <td className="azm-mono whitespace-nowrap">{ev.type}</td>
                        <td className="azm-mono max-w-[150px] truncate" title={ev.resend_email_id ?? ''}>
                          {ev.resend_email_id ?? t('admin.common.none')}
                        </td>
                        <td className="azm-mono max-w-[130px] truncate" title={ev.svix_id}>
                          {ev.svix_id}
                        </td>
                        <td className="whitespace-nowrap text-on-surface-variant">
                          {ev.processed_at ? formatDateTime(ev.processed_at, tz, { seconds: true }) : t('admin.common.none')}
                        </td>
                        <td>
                          <Chip tone={chip.tone} title={ev.result ?? undefined}>
                            {chip.label}
                          </Chip>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )
        }
      </QueryState>
      <div className="mt-4 flex items-center justify-end gap-2 text-sm text-on-surface-variant">
        <span>{t('admin.common.page', { n: cursors.length })}</span>
        <Button variant="outlined" size="sm" icon="chevron_left" disabled={cursors.length <= 1} onClick={() => setCursors((c) => c.slice(0, -1))}>
          {t('admin.common.prev')}
        </Button>
        <Button
          variant="outlined"
          size="sm"
          trailingIcon="chevron_right"
          disabled={!nextCursor || query.isPlaceholderData}
          onClick={() => nextCursor && setCursors((c) => [...c, nextCursor])}
        >
          {t('admin.common.next')}
        </Button>
      </div>
    </>
  );
}
