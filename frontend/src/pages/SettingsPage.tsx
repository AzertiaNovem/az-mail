/**
 * Settings page [WP-F]. PLACEHOLDER written by WP0 so the router (WP-E) builds; WP-F replaces it.
 *
 * Contract (kept by the replacement): `export function SettingsPage()`, no props, mounted by
 * router.tsx at `/settings/:tab`. react-router v8 has no regex params, so this component validates
 * `useParams().tab` itself (SETTINGS_TABS) and renders a not-found state for anything else.
 */
import { useParams } from 'react-router';
import { t } from '@/i18n/zh';

export const SETTINGS_TABS = ['general', 'labels', 'account'] as const;
export type SettingsTab = (typeof SETTINGS_TABS)[number];

export const isSettingsTab = (v: string | undefined): v is SettingsTab =>
  (SETTINGS_TABS as readonly string[]).includes(v ?? '');

export function SettingsPage() {
  const { tab } = useParams();
  return (
    <main className="p-6" data-testid="settings-page">
      <h1 className="text-xl text-on-surface">{t('actions.settings')}</h1>
      <p className="mt-2 text-on-surface-variant">{isSettingsTab(tab) ? tab : t('app.notFound')}</p>
    </main>
  );
}
