/**
 * Admin shell [WP-F]. PLACEHOLDER written by WP0 so the router (WP-E) builds; WP-F replaces it.
 *
 * Contract (kept by the replacement): `export function AdminLayout()`, no props, mounted by
 * router.tsx at `/admin` with the tab pages as child routes; it renders the tab bar and an
 * `<Outlet/>`. Admin access (`Me.is_admin`, from the `['me']` query) is checked here; the server
 * enforces it too (403).
 */
import { Outlet } from 'react-router';
import { t } from '@/i18n/zh';

/** Child route paths under /admin, in tab order. */
export const ADMIN_TABS = ['users', 'aliases', 'domains', 'events', 'outbox', 'stats'] as const;
export type AdminTab = (typeof ADMIN_TABS)[number];

export function AdminLayout() {
  return (
    <main className="p-6" data-testid="admin-layout">
      <h1 className="mb-4 text-xl text-on-surface">{t('actions.admin')}</h1>
      <Outlet />
    </main>
  );
}
