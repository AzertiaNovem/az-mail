/**
 * Account avatar menu [WP-E] (Gmail account card): email, large avatar, greeting, 管理设置,
 * 管理后台 (admins), 退出登录: open compose windows save their pending edits first (the token
 * is revoked afterwards), then POST /api/auth/logout and the local logout even if it fails —
 * a voluntary logout, so the login page shows no "session expired" message.
 */
import { useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { useNavigate } from 'react-router';
import { logout } from '@/api/endpoints';
import { flushAllDrafts } from '@/components/compose/useAutosave';
import { Avatar, Button, Icon, Popover } from '@/components/common';
import { useMe } from '@/components/mail/queries';
import { t } from '@/i18n/zh';
import { signOut as endSession } from '@/stores/auth';

/** How long 退出登录 waits for open drafts to save. */
const DRAFT_FLUSH_TIMEOUT_MS = 5000;

export function AccountMenu() {
  const me = useMe();
  const qc = useQueryClient();
  const navigate = useNavigate();
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  if (!me) return null;
  const name = me.display_name || me.email;

  const signOut = async () => {
    setBusy(true);
    try {
      await endSession(qc, logout, () => flushAllDrafts(DRAFT_FLUSH_TIMEOUT_MS));
    } finally {
      setBusy(false);
      setOpen(false);
    }
  };

  const go = (path: string) => {
    setOpen(false);
    void navigate(path);
  };

  return (
    <Popover
      open={open}
      onOpenChange={setOpen}
      align="end"
      sideOffset={8}
      className="w-[min(360px,calc(100vw-24px))] rounded-[28px] bg-surface-container-low p-4"
      aria-label={t('mail.account.menu', { name })}
      trigger={
        <button
          type="button"
          aria-label={t('mail.account.menu', { name })}
          className="flex size-10 items-center justify-center rounded-full hover:bg-on-surface/8 focus-visible:bg-on-surface/12"
        >
          <Avatar name={me.display_name} email={me.email} size={32} decorative />
        </button>
      }
    >
      <p className="truncate text-center text-sm text-on-surface">{me.email}</p>
      <div className="my-4 flex flex-col items-center gap-3">
        <Avatar name={me.display_name} email={me.email} size={80} decorative />
        <p className="text-[22px] text-on-surface">{t('mail.account.hello', { name: me.display_name || me.email.split('@')[0] || '' })}</p>
        <Button variant="outlined" onClick={() => go('/settings/account')}>
          {t('mail.account.manageSettings')}
        </Button>
      </div>
      <div className="flex flex-col gap-1 overflow-hidden rounded-2xl bg-surface-container">
        {me.is_admin && (
          <button
            type="button"
            onClick={() => go('/admin')}
            className="flex h-12 items-center gap-3 px-5 text-left text-sm text-on-surface hover:bg-hover"
          >
            <Icon name="admin_panel_settings" className="text-on-surface-variant" />
            {t('mail.account.admin')}
          </button>
        )}
        <button
          type="button"
          onClick={() => void signOut()}
          disabled={busy}
          className="flex h-12 items-center gap-3 px-5 text-left text-sm text-on-surface hover:bg-hover disabled:opacity-50"
        >
          <Icon name="logout" className="text-on-surface-variant" />
          {busy ? t('mail.account.loggingOut') : t('mail.account.logout')}
        </button>
      </div>
    </Popover>
  );
}
