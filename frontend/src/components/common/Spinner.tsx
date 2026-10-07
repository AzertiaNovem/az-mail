import { t } from '@/i18n/zh';
import { cx } from './cx';

export interface SpinnerProps {
  /** Pixel size. Default 24. */
  size?: number;
  /** Use the surrounding text color instead of primary blue. */
  inheritColor?: boolean;
  className?: string;
  /** Accessible label; defaults to "正在加载…". */
  label?: string;
  /**
   * Purely visual (no role=status, hidden from assistive tech), e.g. inside a loading Button,
   * where `aria-busy` already conveys the state and the label must not join the button's name.
   */
  decorative?: boolean;
}

/** Indeterminate circular progress. */
export function Spinner({
  size = 24,
  inheritColor = false,
  className,
  label = t('app.loading'),
  decorative = false,
}: SpinnerProps) {
  return (
    <span
      role={decorative ? undefined : 'status'}
      aria-label={decorative ? undefined : label}
      aria-hidden={decorative || undefined}
      className={cx('inline-flex shrink-0', inheritColor ? 'text-current' : 'text-primary', className)}
    >
      <svg className="animate-spin" width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <circle cx="12" cy="12" r="9.5" stroke="currentColor" strokeOpacity="0.2" strokeWidth="3" />
        <path d="M21.5 12A9.5 9.5 0 0 0 12 2.5" stroke="currentColor" strokeWidth="3" strokeLinecap="round" />
      </svg>
    </span>
  );
}
