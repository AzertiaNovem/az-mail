/**
 * Settings page [WP-F] at `/settings/:tab` (general | labels | account), Gmail style: a card
 * with the title, a tab bar and the tab's form.
 *
 * Contract: `export function SettingsPage()`, no props, mounted by router.tsx at
 * `/settings/:tab`. react-router v8 has no regex params, so this component validates
 * `useParams().tab` itself (SETTINGS_TABS) and renders a not-found state for anything else.
 */
import '@/styles/compose.css';
import { Link, NavLink, useParams } from 'react-router';
import { errorMessage } from '@/api/client';
import { Button, Spinner } from '@/components/common';
import { useMe } from '@/components/compose/useMe';
import { AccountSettings } from '@/components/settings/AccountSettings';
import { GeneralSettings } from '@/components/settings/GeneralSettings';
import { LabelsSettings } from '@/components/settings/LabelsSettings';
import { t } from '@/i18n/zh';

export const SETTINGS_TABS = ['general', 'labels', 'account'] as const;
export type SettingsTab = (typeof SETTINGS_TABS)[number];

export const isSettingsTab = (v: string | undefined): v is SettingsTab =>
  (SETTINGS_TABS as readonly string[]).includes(v ?? '');

export function SettingsPage() {
  const { tab } = useParams();
  const me = useMe();
  const valid = isSettingsTab(tab);

  let body;
  if (!valid) {
    body = (
      <div className="azm-page-body flex flex-col items-center gap-3 py-16 text-sm text-on-surface-variant">
        <p className="m-0">{t('settings.notFound')}</p>
        <Link className="text-link hover:underline" to="/settings/general">
          {t('settings.tabs.general')}
        </Link>
      </div>
    );
  } else if (me.isPending) {
    body = (
      <div className="azm-page-body flex justify-center py-16">
        <Spinner />
      </div>
    );
  } else if (!me.data) {
    body = (
      <div className="azm-page-body flex flex-col items-center gap-3 py-16 text-sm text-on-surface-variant">
        <p className="m-0">{t('settings.loadFailed', { reason: errorMessage(me.error) })}</p>
        <Button variant="tonal" size="sm" onClick={() => void me.refetch()}>
          {t('actions.retry')}
        </Button>
      </div>
    );
  } else if (tab === 'general') {
    body = <GeneralSettings me={me.data} />;
  } else if (tab === 'labels') {
    body = <LabelsSettings />;
  } else {
    body = <AccountSettings me={me.data} />;
  }

  return (
    <section className="azm-page" aria-labelledby="settings-title" data-testid="settings-page">
      <header className="azm-page-header">
        <h1 id="settings-title" className="azm-page-title">
          {t('settings.title')}
        </h1>
      </header>
      <nav className="azm-tabs" aria-label={t('settings.tabsLabel')}>
        {SETTINGS_TABS.map((k) => (
          <NavLink key={k} to={`/settings/${k}`} className="azm-tab">
            {t(`settings.tabs.${k}` as const)}
          </NavLink>
        ))}
      </nav>
      {body}
    </section>
  );
}
