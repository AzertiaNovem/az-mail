/**
 * Shared building blocks of the admin pages [WP-F]: status chips, query state (loading /
 * error / empty), a copy-to-clipboard button and the admin's display timezone.
 */
import type { UseQueryResult } from '@tanstack/react-query';
import type { ReactNode } from 'react';
import { errorMessage } from '@/api/client';
import type { OutboundStatus } from '@/api/types';
import { Button, cx, IconButton, Spinner } from '@/components/common';
import { useMe } from '@/components/compose/useMe';
import { statusName, t } from '@/i18n/zh';
import { DEFAULT_TIMEZONE, safeTimeZone } from '@/lib/quote';
import { toast } from '@/stores/toast';

export type ChipTone = 'neutral' | 'info' | 'success' | 'warning' | 'error';

export function Chip({ tone, children, title }: { tone: ChipTone; children: ReactNode; title?: string }) {
  return (
    <span className={cx('azm-chip', `tone-${tone}`)} title={title}>
      {children}
    </span>
  );
}

const STATUS_TONES: Record<OutboundStatus, ChipTone> = {
  queued: 'info',
  sending: 'info',
  accepted: 'info',
  scheduled: 'info',
  sent: 'success',
  delivered: 'success',
  delivery_delayed: 'warning',
  bounced: 'error',
  complained: 'error',
  failed: 'error',
  suppressed: 'error',
  canceled: 'neutral',
};

export function OutboundStatusChip({ status }: { status: OutboundStatus }) {
  return <Chip tone={STATUS_TONES[status] ?? 'neutral'}>{statusName(status) ?? status}</Chip>;
}

/** The admin's display timezone (Settings.timezone). */
export function useAdminTimeZone(): string {
  const me = useMe();
  return me.data ? safeTimeZone(me.data.settings.timezone) : DEFAULT_TIMEZONE;
}

export interface QueryStateProps<T> {
  query: UseQueryResult<T>;
  /** Shown when the data is an empty array. */
  empty?: string;
  children: (data: T) => ReactNode;
}

/** Renders loading / error (with 重试) / empty states around a query's data. */
export function QueryState<T>({ query, empty, children }: QueryStateProps<T>) {
  if (query.isPending) {
    return (
      <div className="flex justify-center py-12">
        <Spinner />
      </div>
    );
  }
  if (query.isError && query.data === undefined) {
    return (
      <div className="azm-banner tone-error" role="alert">
        <span className="flex-1">{t('admin.common.loadFailed', { reason: errorMessage(query.error) })}</span>
        <Button variant="text" size="sm" onClick={() => void query.refetch()}>
          {t('admin.common.retry')}
        </Button>
      </div>
    );
  }
  const data = query.data as T;
  if (empty && Array.isArray(data) && data.length === 0) {
    return <p className="py-12 text-center text-sm text-on-surface-variant">{empty}</p>;
  }
  return <>{children(data)}</>;
}

export async function copyText(text: string): Promise<void> {
  try {
    await navigator.clipboard.writeText(text);
    toast.push({ message: t('toasts.copied'), durationMs: 2000 });
  } catch {
    toast.error(t('errors.unknown'));
  }
}

export function CopyButton({ text, label }: { text: string; label?: string }) {
  return <IconButton icon="content_copy" size="sm" label={label ?? t('admin.common.copy')} onClick={() => void copyText(text)} />;
}

/** Title row of an admin page: actions on the right. */
export function AdminToolbar({ children, actions }: { children?: ReactNode; actions?: ReactNode }) {
  return (
    <div className="mb-4 flex flex-wrap items-center gap-3">
      <div className="flex min-w-0 flex-1 flex-wrap items-center gap-3">{children}</div>
      {actions && <div className="flex shrink-0 items-center gap-2">{actions}</div>}
    </div>
  );
}
