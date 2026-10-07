/**
 * Settings → 账号 [WP-F]: current account info (name, email, role, send-as identities, server)
 * and 修改密码 (current / new / confirm). A wrong current password is 403
 * `invalid_credentials` (it never ends the session); a weak one is 422 `weak_password`.
 */
import { useMutation } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { changePassword } from '@/api/endpoints';
import type { Me } from '@/api/types';
import { Avatar, Button, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';
import { SettingsRow } from './GeneralSettings';
import { hasErrors, validatePasswordChange, type PasswordErrors, type PasswordForm } from './validation';

const EMPTY: PasswordForm = { current: '', next: '', confirm: '' };

export function AccountSettings({ me }: { me: Me }) {
  const [form, setForm] = useState<PasswordForm>(EMPTY);
  const [errors, setErrors] = useState<PasswordErrors>({});
  const [formError, setFormError] = useState<string | null>(null);

  const mutation = useMutation({
    mutationFn: (f: PasswordForm) => changePassword({ current_password: f.current, new_password: f.next }),
    onSuccess: () => {
      setForm(EMPTY);
      setErrors({});
      toast.push({ message: t('settings.account.changed') });
    },
    onError: (e) => {
      if (isApiError(e, 'invalid_credentials')) setErrors({ current: t('settings.account.wrongCurrent') });
      else if (isApiError(e, 'weak_password')) setErrors({ next: errorMessage(e) });
      else setFormError(errorMessage(e));
    },
  });

  const set = (key: keyof PasswordForm, value: string) => {
    setForm((f) => ({ ...f, [key]: value }));
    setErrors((e) => ({ ...e, [key]: undefined }));
    setFormError(null);
  };

  const submit = (e: FormEvent) => {
    e.preventDefault();
    const errs = validatePasswordChange(form);
    setErrors(errs);
    if (hasErrors(errs)) return;
    mutation.mutate(form);
  };

  return (
    <div className="azm-page-body">
      <div className="max-w-4xl">
        <SettingsRow label={t('settings.account.info')}>
          <div className="flex items-start gap-4">
            <Avatar name={me.display_name} email={me.email} size={48} decorative />
            <dl className="m-0 grid grid-cols-[auto_1fr] gap-x-6 gap-y-2 text-sm">
              <dt className="text-on-surface-variant">{t('settings.account.name')}</dt>
              <dd className="m-0">{me.display_name || '—'}</dd>
              <dt className="text-on-surface-variant">{t('settings.account.email')}</dt>
              <dd className="m-0 break-all">{me.email}</dd>
              <dt className="text-on-surface-variant">{t('settings.account.role')}</dt>
              <dd className="m-0">{me.is_admin ? t('settings.account.admin') : t('settings.account.member')}</dd>
              <dt className="text-on-surface-variant">{t('settings.account.identities')}</dt>
              <dd className="m-0">
                <ul className="m-0 flex list-none flex-col gap-1 p-0">
                  {me.identities.map((i) => (
                    <li key={i.address_id} className="flex flex-wrap items-center gap-2">
                      <span className="break-all">{i.display_name ? `${i.display_name} <${i.email}>` : i.email}</span>
                      {i.kind === 'alias' && <span className="azm-chip tone-info">{t('settings.account.aliasTag')}</span>}
                      {i.is_default && <span className="azm-chip tone-neutral">{t('settings.account.defaultTag')}</span>}
                    </li>
                  ))}
                </ul>
              </dd>
              <dt className="text-on-surface-variant">{t('settings.account.version')}</dt>
              <dd className="m-0">{me.server.version || '—'}</dd>
            </dl>
          </div>
        </SettingsRow>

        <SettingsRow label={t('settings.account.password')} help={t('settings.account.passwordHint')}>
          <form className="flex max-w-sm flex-col gap-4" onSubmit={submit} noValidate>
            {/* Lets password managers associate the new password with this account. */}
            <input type="email" name="username" autoComplete="username" value={me.email} readOnly hidden />
            <TextField
              type="password"
              label={t('settings.account.current')}
              autoComplete="current-password"
              value={form.current}
              error={errors.current}
              onChange={(e) => set('current', e.target.value)}
            />
            <TextField
              type="password"
              label={t('settings.account.new')}
              autoComplete="new-password"
              value={form.next}
              error={errors.next}
              onChange={(e) => set('next', e.target.value)}
            />
            <TextField
              type="password"
              label={t('settings.account.confirm')}
              autoComplete="new-password"
              value={form.confirm}
              error={errors.confirm}
              onChange={(e) => set('confirm', e.target.value)}
            />
            {formError && (
              <p role="alert" className="m-0 text-sm text-error">
                {formError}
              </p>
            )}
            <div>
              <Button type="submit" loading={mutation.isPending}>
                {t('settings.account.submit')}
              </Button>
            </div>
          </form>
        </SettingsRow>
      </div>
    </div>
  );
}
