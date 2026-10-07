/**
 * Admin shell [WP-F]: title, tab bar (用户 · 别名 · 域名 · Webhook 事件 · 发件队列 · 统计) and
 * the tab page in an `<Outlet/>`.
 *
 * Contract: `export function AdminLayout()`, no props, mounted by router.tsx at `/admin` with
 * the tab pages as child routes. Admin access (`Me.is_admin`, from the `['me']` query) is
 * checked here; the server enforces it too (403).
 */
import '@/styles/compose.css';
import { Link, NavLink, Outlet } from 'react-router';
import { errorMessage } from '@/api/client';
import { Button, Icon, Spinner } from '@/components/common';
import { useMe } from '@/components/compose/useMe';
import { t } from '@/i18n/zh';

/** Child route paths under /admin, in tab order. */
export const ADMIN_TABS = ['users', 'aliases', 'domains', 'events', 'outbox', 'stats'] as const;
export type AdminTab = (typeof ADMIN_TABS)[number];

export function AdminLayout() {
  const me = useMe();

  let body;
  if (me.isPending) {
    body = (
      <div className="flex justify-center py-16">
        <Spinner />
      </div>
    );
  } else if (!me.data) {
    body = (
      <div className="flex flex-col items-center gap-3 py-16 text-sm text-on-surface-variant">
        <p className="m-0">{t('admin.common.loadFailed', { reason: errorMessage(me.error) })}</p>
        <Button variant="tonal" size="sm" onClick={() => void me.refetch()}>
          {t('admin.common.retry')}
        </Button>
      </div>
    );
  } else if (!me.data.is_admin) {
    body = (
      <div className="flex flex-col items-center gap-3 py-16 text-sm text-on-surface-variant" role="alert">
        <Icon name="lock" size={40} className="text-muted" />
        <p className="m-0">{t('admin.forbidden')}</p>
        <Link className="text-link hover:underline" to="/mail/inbox">
          {t('admin.backToMail')}
        </Link>
      </div>
    );
  }

  const isAdmin = !!me.data?.is_admin;
  return (
    <section className="azm-page" aria-labelledby="admin-title" data-testid="admin-layout">
      <header className="azm-page-header">
        <h1 id="admin-title" className="azm-page-title flex-1">
          {t('admin.title')}
        </h1>
        <Link
          to="/mail/inbox"
          className="inline-flex h-9 items-center gap-1 rounded-full px-3 text-sm font-medium text-primary hover:bg-primary/8"
        >
          <Icon name="arrow_back" size={18} />
          {t('admin.backToMail')}
        </Link>
      </header>
      {isAdmin && (
        <nav className="azm-tabs" aria-label={t('admin.tabsLabel')}>
          {ADMIN_TABS.map((tab) => (
            <NavLink key={tab} to={`/admin/${tab}`} className="azm-tab">
              {t(`admin.tabs.${tab}` as const)}
            </NavLink>
          ))}
        </nav>
      )}
      <div className="azm-page-body">{isAdmin ? <Outlet /> : body}</div>
    </section>
  );
}
