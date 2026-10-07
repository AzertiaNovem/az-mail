import { t } from '@/i18n/zh';

const WIDTHS = ['62%', '48%', '71%', '55%', '66%', '44%', '58%', '69%', '51%', '63%'];

/** Shimmering placeholder rows while a thread list loads. */
export function ListSkeleton({ rows = 10 }: { rows?: number }) {
  return (
    <div role="status" aria-label={t('mail.list.loading')} aria-busy="true">
      {Array.from({ length: rows }, (_, i) => (
        <div key={i} className="flex h-10 items-center gap-4 border-b border-divider px-4" aria-hidden="true">
          <span className="azm-shimmer size-4 rounded-sm" />
          <span className="azm-shimmer size-4 rounded-full" />
          <span className="azm-shimmer h-3 w-[140px] shrink-0 rounded" />
          <span className="azm-shimmer h-3 rounded" style={{ width: WIDTHS[i % WIDTHS.length] }} />
          <span className="azm-shimmer ml-auto h-3 w-10 rounded" />
        </div>
      ))}
    </div>
  );
}

/** Placeholder for a thread view while it loads. */
export function ThreadSkeleton() {
  return (
    <div role="status" aria-label={t('mail.thread.loading')} aria-busy="true" className="px-6 py-5">
      <span className="azm-shimmer mb-6 block h-6 w-2/5 rounded" aria-hidden="true" />
      {[0, 1].map((i) => (
        <div key={i} className="mb-6 flex gap-4" aria-hidden="true">
          <span className="azm-shimmer size-10 shrink-0 rounded-full" />
          <div className="flex-1">
            <span className="azm-shimmer mb-2 block h-3.5 w-40 rounded" />
            <span className="azm-shimmer mb-5 block h-3 w-24 rounded" />
            <span className="azm-shimmer mb-2 block h-3 w-11/12 rounded" />
            <span className="azm-shimmer mb-2 block h-3 w-4/5 rounded" />
            <span className="azm-shimmer block h-3 w-3/5 rounded" />
          </div>
        </div>
      ))}
    </div>
  );
}
