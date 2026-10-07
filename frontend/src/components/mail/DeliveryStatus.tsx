/**
 * Delivery status of an outbound message [WP-E]: a status chip with the Chinese label
 * (待发送 … 已取消, "已定时 10月8日 09:00"), the bounce / failure detail, and a popover with the
 * event timeline (GET /api/messages/:id/events, `['message', id, 'events']`, invalidated by the
 * `outbound.status` WS event). Failed sends can be retried; scheduled ones canceled (the draft
 * reopens in compose).
 */
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { errorMessage } from '@/api/client';
import { cancelSchedule, getMessageEvents, retrySend } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import type { DeliveryEvent, Message, OutboundStatus } from '@/api/types';
import { Button, cx, Icon, Popover, Spinner } from '@/components/common';
import { statusName, t, zh } from '@/i18n/zh';
import { formatFullDate, formatScheduleTime } from '@/lib/format';
import { toast } from '@/stores/toast';
import { openDraft } from './compose';
import { CHIP_TONES, type ChipTone } from './ThreadRow';

export function statusTone(s: OutboundStatus): ChipTone {
  switch (s) {
    case 'delivered':
      return 'success';
    case 'sent':
    case 'accepted':
      return 'neutral';
    case 'scheduled':
      return 'info';
    case 'delivery_delayed':
      return 'warning';
    case 'bounced':
    case 'complained':
    case 'failed':
    case 'suppressed':
      return 'error';
    default:
      return 'neutral';
  }
}

const STATUS_ICON: Record<OutboundStatus, string> = {
  queued: 'schedule',
  sending: 'outbox',
  accepted: 'check',
  scheduled: 'schedule_send',
  sent: 'check',
  delivered: 'done_all',
  delivery_delayed: 'hourglass_top',
  bounced: 'error',
  complained: 'report',
  failed: 'error',
  suppressed: 'block',
  canceled: 'cancel',
};

const SCHEDULE_PENDING = new Set<OutboundStatus>(['queued', 'sending', 'accepted', 'scheduled']);

/** Chip text: "已定时 10月8日 09:00" while a scheduled send is pending, else the status name. */
export function deliveryLabel(outbound: NonNullable<Message['outbound']>, now: number, tz: string): string {
  if (outbound.scheduled_at !== null && SCHEDULE_PENDING.has(outbound.status))
    return statusName('scheduled', formatScheduleTime(outbound.scheduled_at, now, tz));
  return statusName(outbound.status);
}

/** Chinese name of a delivery event type (unknown types are shown as-is). */
export function eventName(type: string): string {
  const key = type.replace(/\./g, '_') as keyof typeof zh.mail.delivery.events;
  const name = zh.mail.delivery.events[key];
  return typeof name === 'string' && key !== 'other' ? name : type;
}

/** Human-readable detail strings of an event (bounce message, reason, …). */
export function eventDetail(e: DeliveryEvent): string | null {
  const d = e.detail ?? {};
  const pick = (v: unknown): string | null => (typeof v === 'string' && v.trim() ? v.trim() : null);
  const bounce = (d as { bounce?: unknown }).bounce;
  return (
    pick((d as Record<string, unknown>).message) ??
    pick((d as Record<string, unknown>).reason) ??
    pick((d as Record<string, unknown>).detail) ??
    pick((d as Record<string, unknown>).error) ??
    (typeof bounce === 'object' && bounce !== null ? pick((bounce as Record<string, unknown>).message) : null)
  );
}

function Timeline({ messageId, tz }: { messageId: number; tz: string }) {
  const q = useQuery({
    queryKey: queryKeys.messageEvents(messageId),
    queryFn: ({ signal }) => getMessageEvents(messageId, signal),
    staleTime: staleTimes.messageEvents,
  });
  if (q.isPending)
    return (
      <div className="flex items-center gap-2 py-2 text-on-surface-variant">
        <Spinner size={16} /> {t('mail.delivery.loading')}
      </div>
    );
  if (q.isError) return <p className="py-2 text-error">{t('mail.delivery.loadFailed')}</p>;
  const events = [...(q.data?.events ?? [])].sort((a, b) => a.occurred_at - b.occurred_at);
  if (events.length === 0) return <p className="py-2 text-on-surface-variant">{t('mail.delivery.empty')}</p>;
  return (
    <ol className="relative ml-1.5 border-l border-divider pl-4">
      {events.map((e, i) => {
        const detail = eventDetail(e);
        return (
          <li key={`${e.type}-${e.occurred_at}-${i}`} className="relative pb-3 last:pb-0">
            <span className="absolute -left-[21px] top-1.5 size-2.5 rounded-full border-2 border-surface-container bg-primary" aria-hidden="true" />
            <p className="text-sm text-on-surface">{eventName(e.type)}</p>
            <p className="text-xs text-on-surface-variant">{formatFullDate(e.occurred_at, tz)}</p>
            {detail && <p className="mt-0.5 break-words text-xs text-on-surface-variant">{detail}</p>}
          </li>
        );
      })}
    </ol>
  );
}

export interface DeliveryStatusProps {
  message: Message;
  now: number;
  tz: string;
}

export function DeliveryStatus({ message, now, tz }: DeliveryStatusProps) {
  const qc = useQueryClient();
  const [busy, setBusy] = useState(false);
  const ob = message.outbound;
  if (!ob || message.direction !== 'out' || message.is_draft) return null;
  const tone = ob.scheduled_at !== null && SCHEDULE_PENDING.has(ob.status) ? 'info' : statusTone(ob.status);
  const label = deliveryLabel(ob, now, tz);
  const canRetry = ob.status === 'failed';
  const canCancel = ob.scheduled_at !== null && SCHEDULE_PENDING.has(ob.status) && ob.scheduled_at > now;

  const retry = async () => {
    setBusy(true);
    try {
      await retrySend(message.id);
      toast.push({ message: t('mail.delivery.retried') });
      void qc.invalidateQueries({ queryKey: queryKeys.thread(message.thread_id) });
      void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  const cancel = async () => {
    setBusy(true);
    try {
      const { draft } = await cancelSchedule(message.id);
      // The draft cache is never invalidated: seed it so the compose window starts from this version.
      qc.setQueryData(queryKeys.draft(draft.id), draft);
      void qc.invalidateQueries({ queryKey: queryKeys.thread(message.thread_id) });
      void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
      void qc.invalidateQueries({ queryKey: queryKeys.counts() });
      toast.push({ message: t('mail.delivery.canceled') });
      openDraft(draft.id);
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <span className="inline-flex flex-wrap items-center gap-2">
      <Popover
        align="start"
        className="w-80 p-4"
        aria-label={t('mail.delivery.timeline')}
        trigger={
          <button
            type="button"
            className={cx('inline-flex h-6 items-center gap-1 rounded-full px-2 text-xs font-medium transition-[filter] hover:brightness-95', CHIP_TONES[tone])}
            aria-label={`${t('mail.delivery.title')}：${label}`}
          >
            <Icon name={STATUS_ICON[ob.status]} size={14} />
            {label}
            <Icon name="arrow_drop_down" size={16} className="-mr-1" />
          </button>
        }
      >
        <p className="mb-1 text-sm font-medium text-on-surface">{t('mail.delivery.timeline')}</p>
        <p className={cx('mb-3 text-xs', tone === 'error' ? 'text-error' : 'text-on-surface-variant')}>
          {label}
          {ob.status_detail ? ` · ${ob.status_detail}` : ''}
        </p>
        <Timeline messageId={message.id} tz={tz} />
      </Popover>
      {ob.status_detail && tone === 'error' && <span className="max-w-[420px] truncate text-xs text-error" title={ob.status_detail}>{ob.status_detail}</span>}
      {canRetry && (
        <Button variant="text" size="sm" icon="refresh" loading={busy} onClick={() => void retry()}>
          {busy ? t('mail.delivery.retrying') : t('mail.delivery.retry')}
        </Button>
      )}
      {canCancel && (
        <Button variant="text" size="sm" icon="cancel_schedule_send" loading={busy} onClick={() => void cancel()}>
          {t('mail.delivery.cancelSchedule')}
        </Button>
      )}
    </span>
  );
}
