/**
 * Route tree [WP-E] (DESIGN.md §5 Routes). Contract kept from the WP0 placeholder: export
 * `router` (a data router from `createBrowserRouter`); main.tsx uses `router.navigate('/login?next=…')`
 * when the session ends. LoginPage only follows `next` values that start with a single '/'.
 *
 *   /                                  → /mail/inbox
 *   /login                             LoginPage
 *   (RequireAuth → AppShell)
 *     /mail                            → /mail/inbox
 *     /mail/search[/:threadId]?q=      MailPage search
 *     /mail/label/:labelId[/:threadId] MailPage label
 *     /mail/:folder[/:threadId]        MailPage folder (validates FOLDER_IDS → NotFound)
 *     /settings                        → /settings/general
 *     /settings/:tab                   SettingsPage [F] (validates :tab itself)
 *     /admin/{users,aliases,domains,events,outbox,stats}  AdminLayout + tab pages [F]
 *   *                                  NotFound
 *
 * Settings and admin routes are lazy (`lazy: () => import(…)`, the optional code splitting the
 * WP0 placeholder allowed); their exports keep the WP0 names.
 *
 * react-router v8 has no regex params: static segments (search, label) outrank `:folder`, and
 * the components validate their params.
 */
import { createBrowserRouter, redirect, type RouteObject } from 'react-router';
import { AppShell } from '@/components/layout/AppShell';
import { FullPageLoading, RequireAuth } from '@/components/layout/RequireAuth';
import { LoginPage } from '@/pages/LoginPage';
import { MailPage } from '@/pages/MailPage';
import { NotFound } from '@/pages/NotFound';

// Settings and admin [F] are code-split: the mail UI does not pay for them up front.
const settingsPage = () => import('@/pages/SettingsPage').then((m) => ({ Component: m.SettingsPage }));
const adminLayout = () => import('@/pages/admin/AdminLayout').then((m) => ({ Component: m.AdminLayout }));
const adminUsers = () => import('@/pages/admin/Users').then((m) => ({ Component: m.AdminUsers }));
const adminAliases = () => import('@/pages/admin/Aliases').then((m) => ({ Component: m.AdminAliases }));
const adminDomains = () => import('@/pages/admin/Domains').then((m) => ({ Component: m.AdminDomains }));
const adminEvents = () => import('@/pages/admin/Events').then((m) => ({ Component: m.AdminEvents }));
const adminOutbox = () => import('@/pages/admin/Outbox').then((m) => ({ Component: m.AdminOutbox }));
const adminStats = () => import('@/pages/admin/Stats').then((m) => ({ Component: m.AdminStats }));

export const routes: RouteObject[] = [
  { path: '/', loader: () => redirect('/mail/inbox') },
  { path: '/login', element: <LoginPage /> },
  {
    element: <RequireAuth />,
    // Shown while a lazy route loads on a direct visit (e.g. a bookmarked /admin/users).
    HydrateFallback: FullPageLoading,
    children: [
      {
        element: <AppShell />,
        children: [
          { path: '/mail', loader: () => redirect('/mail/inbox') },
          { path: '/mail/search', element: <MailPage kind="search" /> },
          { path: '/mail/search/:threadId', element: <MailPage kind="search" /> },
          { path: '/mail/label/:labelId', element: <MailPage kind="label" /> },
          { path: '/mail/label/:labelId/:threadId', element: <MailPage kind="label" /> },
          { path: '/mail/:folder', element: <MailPage kind="folder" /> },
          { path: '/mail/:folder/:threadId', element: <MailPage kind="folder" /> },
          { path: '/settings', loader: () => redirect('/settings/general') },
          { path: '/settings/:tab', lazy: settingsPage },
          {
            path: '/admin',
            lazy: adminLayout,
            children: [
              { index: true, loader: () => redirect('/admin/users') },
              { path: 'users', lazy: adminUsers },
              { path: 'aliases', lazy: adminAliases },
              { path: 'domains', lazy: adminDomains },
              { path: 'events', lazy: adminEvents },
              { path: 'outbox', lazy: adminOutbox },
              { path: 'stats', lazy: adminStats },
            ],
          },
        ],
      },
    ],
  },
  { path: '*', element: <NotFound /> },
];

export const router = createBrowserRouter(routes);
