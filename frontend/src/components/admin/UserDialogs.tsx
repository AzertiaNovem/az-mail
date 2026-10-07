/**
 * Admin user dialogs [WP-F]: 创建用户 (address = local part + domain, display name, initial
 * password, admin) and 修改用户 (display name, admin, disabled, reset password). Server errors
 * land under the field they concern (address_exists, unknown_domain, weak_password, last_admin).
 */
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { useState, type FormEvent, type ReactNode } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { adminCreateUser, adminKeys, adminUpdateUser } from '@/api/admin';
import type { AdminUser, AdminUserPatch, DomainRow } from '@/api/types';
import { Button, Dialog, IconButton, Select, Switch, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';
import {
  ADMIN_NAME_MAX,
  hasFieldErrors,
  validateUserCreate,
  validateUserEdit,
  type FieldErrors,
  type UserCreateForm,
  type UserEditForm,
} from './validation';

function PasswordField({
  label,
  value,
  onChange,
  error,
  helperText,
  autoComplete,
}: {
  label: string;
  value: string;
  onChange: (v: string) => void;
  error?: string;
  helperText?: string;
  autoComplete: string;
}) {
  const [show, setShow] = useState(false);
  return (
    <TextField
      label={label}
      type={show ? 'text' : 'password'}
      value={value}
      autoComplete={autoComplete}
      error={error}
      helperText={helperText}
      onChange={(e) => onChange(e.target.value)}
      trailing={
        <IconButton
          icon={show ? 'visibility_off' : 'visibility'}
          size="sm"
          label={show ? t('admin.common.hidePassword') : t('admin.common.showPassword')}
          tooltip={false}
          className="-mr-2"
          onClick={() => setShow((s) => !s)}
        />
      }
    />
  );
}

function FormError({ children }: { children: ReactNode }) {
  return (
    <p role="alert" className="m-0 rounded-lg bg-error-container px-3 py-2 text-sm text-on-error-container">
      {children}
    </p>
  );
}

// ───────────── create ─────────────

export function UserCreateDialog({ domains, onClose }: { domains: DomainRow[]; onClose: () => void }) {
  const qc = useQueryClient();
  const [form, setForm] = useState<UserCreateForm>({
    localPart: '',
    domain: domains[0]?.name ?? '',
    displayName: '',
    password: '',
    isAdmin: false,
  });
  const [errors, setErrors] = useState<FieldErrors<UserCreateForm>>({});
  const [formError, setFormError] = useState<string | null>(null);

  const mutation = useMutation({
    mutationFn: (f: UserCreateForm) =>
      adminCreateUser({
        email: `${f.localPart.trim().toLowerCase()}@${f.domain}`,
        display_name: f.displayName.trim(),
        password: f.password,
        is_admin: f.isAdmin,
      }),
    onSuccess: (u) => {
      void qc.invalidateQueries({ queryKey: adminKeys.all() });
      toast.push({ message: t('admin.users.created', { email: u.email }) });
      onClose();
    },
    onError: (e) => {
      if (isApiError(e, 'address_exists')) setErrors({ localPart: errorMessage(e) });
      else if (isApiError(e, 'unknown_domain')) setErrors({ domain: errorMessage(e) });
      else if (isApiError(e, 'weak_password')) setErrors({ password: errorMessage(e) });
      else setFormError(errorMessage(e));
    },
  });

  const set = <K extends keyof UserCreateForm>(k: K, v: UserCreateForm[K]) => {
    setForm((f) => ({ ...f, [k]: v }));
    setErrors((e) => ({ ...e, [k]: undefined }));
    setFormError(null);
  };

  const submit = (e?: FormEvent) => {
    e?.preventDefault();
    const errs = validateUserCreate(form);
    setErrors(errs);
    if (hasFieldErrors(errs)) return;
    mutation.mutate(form);
  };

  return (
    <Dialog
      open
      onOpenChange={(o) => !o && !mutation.isPending && onClose()}
      dismissible={!mutation.isPending}
      title={t('admin.users.createTitle')}
      size="md"
      footer={
        <>
          <Button variant="text" onClick={onClose} disabled={mutation.isPending}>
            {t('actions.cancel')}
          </Button>
          <Button onClick={() => submit()} loading={mutation.isPending} disabled={domains.length === 0}>
            {t('actions.create')}
          </Button>
        </>
      }
    >
      <form className="flex flex-col gap-4" onSubmit={submit} noValidate>
        {domains.length === 0 && <FormError>{t('admin.users.noDomains')}</FormError>}
        <div className="grid grid-cols-[1fr_auto_minmax(0,1fr)] items-start gap-2">
          <TextField
            label={t('admin.users.address')}
            placeholder={t('admin.users.localPlaceholder')}
            value={form.localPart}
            autoFocus
            autoCapitalize="off"
            spellCheck={false}
            error={errors.localPart}
            onChange={(e) => set('localPart', e.target.value)}
          />
          <span className="pt-8 text-on-surface-variant">@</span>
          <Select
            label={t('admin.users.domain')}
            options={domains.map((d) => ({ value: d.name, label: d.name }))}
            value={form.domain}
            error={errors.domain}
            onValueChange={(v) => set('domain', v)}
            disabled={domains.length === 0}
          />
        </div>
        <TextField
          label={t('admin.users.displayName')}
          value={form.displayName}
          maxLength={ADMIN_NAME_MAX + 20}
          error={errors.displayName}
          onChange={(e) => set('displayName', e.target.value)}
        />
        <PasswordField
          label={t('admin.users.password')}
          value={form.password}
          autoComplete="new-password"
          error={errors.password}
          helperText={t('admin.users.passwordHelp')}
          onChange={(v) => set('password', v)}
        />
        <div className="flex flex-col gap-1">
          <Switch checked={form.isAdmin} onCheckedChange={(v) => set('isAdmin', v)} label={t('admin.users.isAdmin')} />
          <p className="m-0 pl-[64px] text-xs text-on-surface-variant">{t('admin.users.isAdminHelp')}</p>
        </div>
        {formError && <FormError>{formError}</FormError>}
        <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
      </form>
    </Dialog>
  );
}

// ───────────── edit ─────────────

/** PATCH body with only the changed fields. */
export function userPatch(user: AdminUser, f: UserEditForm): AdminUserPatch {
  const patch: AdminUserPatch = {};
  if (f.displayName.trim() !== user.display_name) patch.display_name = f.displayName.trim();
  if (f.isAdmin !== user.is_admin) patch.is_admin = f.isAdmin;
  if (f.disabled !== user.disabled) patch.disabled = f.disabled;
  if (f.password) patch.password = f.password;
  return patch;
}

export function UserEditDialog({ user, isSelf, onClose }: { user: AdminUser; isSelf: boolean; onClose: () => void }) {
  const qc = useQueryClient();
  const [form, setForm] = useState<UserEditForm>({
    displayName: user.display_name,
    isAdmin: user.is_admin,
    disabled: user.disabled,
    password: '',
  });
  const [errors, setErrors] = useState<FieldErrors<UserEditForm>>({});
  const [formError, setFormError] = useState<string | null>(null);

  const mutation = useMutation({
    mutationFn: (patch: AdminUserPatch) => adminUpdateUser(user.id, patch),
    onSuccess: (u) => {
      void qc.invalidateQueries({ queryKey: adminKeys.all() });
      toast.push({ message: t('admin.users.updated', { email: u.email }) });
      onClose();
    },
    onError: (e) => {
      if (isApiError(e, 'last_admin')) setErrors({ isAdmin: errorMessage(e) });
      else if (isApiError(e, 'weak_password')) setErrors({ password: errorMessage(e) });
      else setFormError(errorMessage(e));
    },
  });

  const set = <K extends keyof UserEditForm>(k: K, v: UserEditForm[K]) => {
    setForm((f) => ({ ...f, [k]: v }));
    setErrors((e) => ({ ...e, [k]: undefined }));
    setFormError(null);
  };

  const patch = userPatch(user, form);
  const dirty = Object.keys(patch).length > 0;

  const submit = (e?: FormEvent) => {
    e?.preventDefault();
    const errs = validateUserEdit(form);
    setErrors(errs);
    if (hasFieldErrors(errs)) return;
    if (!dirty) {
      onClose();
      return;
    }
    mutation.mutate(patch);
  };

  return (
    <Dialog
      open
      onOpenChange={(o) => !o && !mutation.isPending && onClose()}
      dismissible={!mutation.isPending}
      title={t('admin.users.editTitle', { email: user.email })}
      size="md"
      footer={
        <>
          <Button variant="text" onClick={onClose} disabled={mutation.isPending}>
            {t('actions.cancel')}
          </Button>
          <Button onClick={() => submit()} loading={mutation.isPending} disabled={!dirty}>
            {t('actions.save')}
          </Button>
        </>
      }
    >
      <form className="flex flex-col gap-5" onSubmit={submit} noValidate>
        <TextField
          label={t('admin.users.displayName')}
          value={form.displayName}
          autoFocus
          maxLength={ADMIN_NAME_MAX + 20}
          error={errors.displayName}
          onChange={(e) => set('displayName', e.target.value)}
        />
        <div className="flex flex-col gap-1">
          <Switch checked={form.isAdmin} onCheckedChange={(v) => set('isAdmin', v)} label={t('admin.users.isAdmin')} />
          <p className={errors.isAdmin ? 'm-0 pl-[64px] text-xs text-error' : 'm-0 pl-[64px] text-xs text-on-surface-variant'}>
            {errors.isAdmin ?? t('admin.users.isAdminHelp')}
          </p>
        </div>
        <div className="flex flex-col gap-1">
          <Switch
            checked={form.disabled}
            disabled={isSelf}
            onCheckedChange={(v) => set('disabled', v)}
            label={t('admin.users.disabledLabel')}
          />
          <p className="m-0 pl-[64px] text-xs text-on-surface-variant">
            {isSelf ? t('admin.users.cannotDisableSelf') : t('admin.users.disabledHelp')}
          </p>
        </div>
        <PasswordField
          label={t('admin.users.resetPassword')}
          value={form.password}
          autoComplete="new-password"
          error={errors.password}
          helperText={t('admin.users.resetPasswordHelp')}
          onChange={(v) => set('password', v)}
        />
        {formError && <FormError>{formError}</FormError>}
        <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
      </form>
    </Dialog>
  );
}
