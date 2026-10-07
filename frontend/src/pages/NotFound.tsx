import { Link } from 'react-router';
import { cx, Icon } from '@/components/common';
import { Logo } from '@/components/layout/Logo';
import { t } from '@/i18n/zh';

/**
 * 404 [WP-E]. `embedded` renders inside the mail card (unknown folder / label / thread id);
 * otherwise a full page (unknown route).
 */
export function NotFound({ embedded = false }: { embedded?: boolean }) {
  const card = (
    <div className="flex flex-col items-center px-6 py-16 text-center">
      <div className="mb-5 flex size-20 items-center justify-center rounded-full bg-surface-container-high text-primary">
        <Icon name="travel_explore" size={40} />
      </div>
      <h1 className="text-[22px] text-on-surface">{t('mail.notFound.title')}</h1>
      <p className="mt-2 max-w-sm text-sm text-on-surface-variant">{t('mail.notFound.message')}</p>
      <Link
        to="/mail/inbox"
        className="mt-6 inline-flex h-10 items-center gap-2 rounded-full bg-primary px-6 text-sm font-medium text-on-primary hover:bg-primary-hover hover:shadow-elevation-1"
      >
        <Icon name="inbox" size={18} />
        {t('mail.notFound.back')}
      </Link>
    </div>
  );
  if (embedded) return card;
  return (
    <main className={cx('flex min-h-dvh flex-col items-center justify-center bg-surface p-4')}>
      <div className="w-full max-w-lg rounded-[28px] bg-surface-container shadow-elevation-1">
        <div className="flex justify-center pt-10">
          <Logo />
        </div>
        {card}
      </div>
    </main>
  );
}
