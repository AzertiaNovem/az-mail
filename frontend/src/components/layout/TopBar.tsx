/**
 * Top bar [WP-E]: menu toggle, AZ Mail logo, search box, realtime-offline hint, shortcuts,
 * settings gear (→ /settings/general) and the account menu.
 */
import { Link, useNavigate } from 'react-router';
import { Icon, IconButton, Tooltip } from '@/components/common';
import { t } from '@/i18n/zh';
import { useUiStore } from '@/stores/ui';
import { useSocketStore } from '@/ws/socket';
import { AccountMenu } from './AccountMenu';
import { Logo } from './Logo';
import { SearchBox } from './SearchBox';

export interface TopBarProps {
  onMenu: () => void;
}

function OfflineHint() {
  // Shown only once a connection attempt has failed (not during the first connect).
  const offline = useSocketStore((s) => s.status === 'closed' && s.attempt > 1);
  if (!offline) return null;
  return (
    <Tooltip label={t('mail.shell.offline')}>
      <span className="flex size-10 items-center justify-center text-on-surface-variant" role="status" aria-label={t('mail.shell.offline')}>
        <Icon name="cloud_off" size={20} />
      </span>
    </Tooltip>
  );
}

export function TopBar({ onMenu }: TopBarProps) {
  const navigate = useNavigate();
  return (
    <header className="flex h-16 shrink-0 items-center gap-1 px-2 sm:gap-2 sm:pr-4">
      <IconButton icon="menu" label={t('mail.shell.mainMenu')} size="lg" iconSize={24} onClick={onMenu} />
      <Link to="/mail/inbox" className="mr-2 hidden shrink-0 items-center rounded-lg py-1 pr-2 sm:flex lg:w-[188px]" aria-label={t('mail.shell.logo')}>
        <Logo />
      </Link>
      <div className="min-w-0 max-w-[720px] flex-1">
        <SearchBox />
      </div>
      <div className="ml-auto flex shrink-0 items-center">
        <OfflineHint />
        <span className="hidden md:inline-flex">
          <IconButton
            icon="keyboard"
            label={t('mail.shell.shortcuts')}
            shortcut="?"
            onClick={() => useUiStore.getState().setShortcutsHelpOpen(true)}
          />
        </span>
        <IconButton icon="settings" label={t('mail.shell.settings')} onClick={() => void navigate('/settings/general')} />
        <AccountMenu />
      </div>
    </header>
  );
}
