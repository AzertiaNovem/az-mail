import { IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import { formatCount } from '@/lib/format';

export interface PagerProps {
  /** 0-based index of the first row on this page. */
  offset: number;
  /** Rows on this page. */
  count: number;
  /** Total when known (null for search). */
  total: number | null;
  hasNewer: boolean;
  hasOlder: boolean;
  onNewer: () => void;
  onOlder: () => void;
}

/** Text of the pager range: "第 1-50 行，共 1,234 行" / "第 51-100 行". */
export function pagerLabel(offset: number, count: number, total: number | null, hasOlder: boolean): string {
  if (count === 0) return '';
  const from = formatCount(offset + 1);
  const to = formatCount(offset + count);
  // On the last page the total is known even when the server does not send it (search).
  const known = total ?? (hasOlder ? null : offset + count);
  return known === null
    ? t('mail.list.pageRangeOpen', { from, to })
    : t('mail.list.pageRange', { from, to, total: formatCount(known) });
}

/** Gmail pager: range text + 较新 / 较旧 chevrons (cursor stack, not infinite scroll). */
export function Pager({ offset, count, total, hasNewer, hasOlder, onNewer, onOlder }: PagerProps) {
  const label = pagerLabel(offset, count, total, hasOlder);
  return (
    <div className="flex shrink-0 items-center gap-1 text-xs text-on-surface-variant">
      {label && (
        <span className="hidden whitespace-nowrap px-2 sm:inline" aria-live="polite">
          {label}
        </span>
      )}
      <IconButton icon="chevron_left" label={t('mail.list.newer')} size="sm" disabled={!hasNewer} onClick={onNewer} />
      <IconButton icon="chevron_right" label={t('mail.list.older')} size="sm" disabled={!hasOlder} onClick={onOlder} />
    </div>
  );
}
