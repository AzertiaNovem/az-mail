import { useId } from 'react';
import { cx } from '@/components/common';
import { t } from '@/i18n/zh';

/** AZ Mail envelope mark (original artwork; no third-party logos). */
export function LogoMark({ size = 32, className }: { size?: number; className?: string }) {
  // Unique gradient ids: a hidden duplicate would otherwise break the visible mark's fills.
  const id = useId().replace(/:/g, '');
  return (
    <svg width={size} height={(size * 3) / 4} viewBox="0 0 40 30" aria-hidden="true" className={cx('shrink-0', className)}>
      <defs>
        <linearGradient id={`${id}-body`} x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#4f8ef7" />
          <stop offset="1" stopColor="#0b57d0" />
        </linearGradient>
        <linearGradient id={`${id}-flap`} x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor="#ea4335" />
          <stop offset="0.5" stopColor="#fbbc04" />
          <stop offset="1" stopColor="#34a853" />
        </linearGradient>
      </defs>
      <rect x="1" y="1" width="38" height="28" rx="5" fill={`url(#${id}-body)`} />
      <path
        d="M4 6.5 20 18 36 6.5"
        fill="none"
        stroke={`url(#${id}-flap)`}
        strokeWidth="3.4"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      <path d="M5 25 15 16.5M35 25 25 16.5" fill="none" stroke="#ffffff" strokeOpacity="0.55" strokeWidth="1.6" strokeLinecap="round" />
    </svg>
  );
}

/** Mark + "AZ Mail" wordmark. */
export function Logo({ showText = true }: { showText?: boolean }) {
  return (
    <span className="inline-flex items-center gap-2.5">
      <LogoMark size={34} />
      {showText && <span className="whitespace-nowrap text-[22px] leading-none tracking-tight text-on-surface-variant">{t('mail.shell.logo')}</span>}
    </span>
  );
}
