/**
 * PLACEHOLDER router so the scaffold builds and runs. WP-E replaces this file with the real
 * route tree (DESIGN.md §5 Routes). Contract kept by the replacement: export `router`
 * (a data router from `createBrowserRouter`) — main.tsx uses `router.navigate('/login?next=…')`
 * when the session ends (`next` only after an expired / revoked session). LoginPage must only
 * follow `next` values that start with a single '/'.
 *
 * Cross-package modules (named exports only, no default exports; each owner replaces the WP0
 * placeholder but keeps the name and signature):
 *   [F] pages/SettingsPage.tsx             export function SettingsPage()
 *         route `/settings/:tab`; reads `useParams().tab` and validates it (SETTINGS_TABS).
 *   [F] pages/admin/AdminLayout.tsx        export function AdminLayout()
 *         route `/admin` (layout: tab bar + `<Outlet/>`, checks `Me.is_admin`).
 *   [F] pages/admin/Users.tsx … Stats.tsx  export function AdminUsers() | AdminAliases() |
 *         AdminDomains() | AdminEvents() | AdminOutbox() | AdminStats()
 *         child routes `users aliases domains events outbox stats` (ADMIN_TABS) of `/admin`.
 *   [F] components/compose/ComposeDock.tsx export function ComposeDock()
 *         no props; reads useComposeStore; AppShell renders it exactly once.
 *   [E] lib/sanitize.ts                    export function sanitizeEmailHtml(html: string,
 *         opts: { allowRemote: boolean; filesOrigins: string[] }): { html: string; blockedImages: number }
 *         used by EmailFrame [E] and by lib/quote.ts [F] for `quoted_html`.
 *   (Optional code splitting: `lazy: () => import('@/pages/SettingsPage').then((m) => ({ Component: m.SettingsPage }))`.)
 *
 * react-router: v8 is installed (DESIGN.md §5 says v7) — data mode, `createBrowserRouter`,
 * `RouterProvider` from 'react-router/dom'. v8 has no regex params, so
 * `/settings/:tab(general|labels|account)` is written `/settings/:tab` (SettingsPage validates
 * `:tab`), `/mail/:folder` must be validated by the mail page (FOLDER_IDS) with a NotFound
 * fallback, and admin tabs are static child paths (unknown ones fall through to `*`).
 */
import '@/styles/mail.css';
import type { ReactNode } from 'react';
import { createBrowserRouter, Link, Outlet, redirect, useNavigate, useParams } from 'react-router';
import { Button, Icon } from '@/components/common';
import { ComposeDock } from '@/components/compose/ComposeDock';
import { FOLDER_IDS, type FolderId } from '@/api/types';
import { folderName, t } from '@/i18n/zh';
import { AdminAliases } from '@/pages/admin/Aliases';
import { AdminDomains } from '@/pages/admin/Domains';
import { AdminEvents } from '@/pages/admin/Events';
import { AdminLayout } from '@/pages/admin/AdminLayout';
import { AdminOutbox } from '@/pages/admin/Outbox';
import { AdminStats } from '@/pages/admin/Stats';
import { AdminUsers } from '@/pages/admin/Users';
import { SettingsPage } from '@/pages/SettingsPage';

function Placeholder({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <main className="flex h-full items-center justify-center p-6">
      <div className="w-full max-w-md rounded-card bg-surface-container p-8 text-center shadow-elevation-1">
        <div className="mb-4 flex items-center justify-center gap-2 text-primary">
          <Icon name="mail" size={32} fill />
          <span className="text-2xl text-on-surface">{t('app.name')}</span>
        </div>
        <h1 className="mb-2 text-lg text-on-surface">{title}</h1>
        {children}
      </div>
    </main>
  );
}

function LoginPlaceholder() {
  return (
    <Placeholder title={t('actions.login')}>
      <p className="text-on-surface-variant">登录页面由 WP-E 实现。</p>
    </Placeholder>
  );
}

function MailPlaceholder() {
  const { folder } = useParams();
  const navigate = useNavigate();
  const known = FOLDER_IDS.includes(folder as FolderId);
  return (
    <Placeholder title={known ? folderName(folder as FolderId) : (folder ?? '')}>
      <p className="mb-4 text-on-surface-variant">邮件界面由 WP-E 实现。</p>
      <Button variant="tonal" icon="login" onClick={() => void navigate('/login')}>
        {t('actions.login')}
      </Button>
    </Placeholder>
  );
}

/** Stand-in for AppShell [E]: the authenticated layout renders its page and ComposeDock once. */
function ShellPlaceholder() {
  return (
    <>
      <Outlet />
      <ComposeDock />
    </>
  );
}

function NotFoundPlaceholder() {
  return (
    <Placeholder title={t('app.notFound')}>
      <Link className="text-link hover:underline" to="/mail/inbox">
        {t('app.backHome')}
      </Link>
    </Placeholder>
  );
}

export const router = createBrowserRouter([
  { path: '/', loader: () => redirect('/mail/inbox') },
  { path: '/login', element: <LoginPlaceholder /> },
  {
    element: <ShellPlaceholder />,
    children: [
      { path: '/mail/:folder', element: <MailPlaceholder /> },
      { path: '/settings', loader: () => redirect('/settings/general') },
      { path: '/settings/:tab', element: <SettingsPage /> },
      {
        path: '/admin',
        element: <AdminLayout />,
        children: [
          { index: true, loader: () => redirect('/admin/users') },
          { path: 'users', element: <AdminUsers /> },
          { path: 'aliases', element: <AdminAliases /> },
          { path: 'domains', element: <AdminDomains /> },
          { path: 'events', element: <AdminEvents /> },
          { path: 'outbox', element: <AdminOutbox /> },
          { path: 'stats', element: <AdminStats /> },
        ],
      },
    ],
  },
  { path: '*', element: <NotFoundPlaceholder /> },
]);
