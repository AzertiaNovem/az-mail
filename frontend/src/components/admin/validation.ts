/**
 * Admin form validation [WP-F] (pure, unit-tested): user create / edit, alias, domain.
 * The server re-validates (409 address_exists, 422 unknown_domain / weak_password, …).
 */
import { t } from '@/i18n/zh';

export const LOCAL_PART_MAX = 64;
export const ADMIN_NAME_MAX = 100;
export const ADMIN_PASSWORD_MIN = 8;

const LOCAL_RE = /^[A-Za-z0-9_+-]+(\.[A-Za-z0-9_+-]+)*$/;
const DOMAIN_RE = /^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+(?:[A-Za-z]{2,63}|xn--[A-Za-z0-9-]{1,59})$/;

export function validateLocalPart(local: string): string | null {
  const v = local.trim();
  if (!v) return t('admin.validation.localRequired');
  if (v.length > LOCAL_PART_MAX) return t('admin.validation.localTooLong');
  if (!LOCAL_RE.test(v)) return t('admin.validation.localInvalid');
  return null;
}

export function validateDomainName(name: string): string | null {
  const v = name.trim().toLowerCase();
  if (!v || !DOMAIN_RE.test(v)) return t('admin.validation.domainInvalid');
  return null;
}

function validateName(name: string, required: boolean): string | null {
  const v = name.trim();
  if (!v) return required ? t('admin.validation.nameRequired') : null;
  if (Array.from(v).length > ADMIN_NAME_MAX) return t('admin.validation.nameTooLong', { max: ADMIN_NAME_MAX });
  return null;
}

function validatePassword(password: string, required: boolean): string | null {
  if (!password) return required ? t('admin.validation.passwordRequired') : null;
  if (Array.from(password).length < ADMIN_PASSWORD_MIN) return t('admin.validation.passwordTooShort', { min: ADMIN_PASSWORD_MIN });
  return null;
}

export interface UserCreateForm {
  localPart: string;
  domain: string;
  displayName: string;
  password: string;
  isAdmin: boolean;
}

export type FieldErrors<F> = Partial<Record<keyof F, string>>;

function compact<F>(e: Record<keyof F, string | null>): FieldErrors<F> {
  const out: FieldErrors<F> = {};
  for (const k of Object.keys(e) as (keyof F)[]) if (e[k]) out[k] = e[k] as string;
  return out;
}

export function validateUserCreate(f: UserCreateForm): FieldErrors<UserCreateForm> {
  return compact<UserCreateForm>({
    localPart: validateLocalPart(f.localPart),
    domain: f.domain ? null : t('admin.validation.domainRequired'),
    displayName: validateName(f.displayName, true),
    password: validatePassword(f.password, true),
    isAdmin: null,
  });
}

export interface UserEditForm {
  displayName: string;
  isAdmin: boolean;
  disabled: boolean;
  /** Empty = keep the current password. */
  password: string;
}

export function validateUserEdit(f: UserEditForm): FieldErrors<UserEditForm> {
  return compact<UserEditForm>({
    displayName: validateName(f.displayName, true),
    isAdmin: null,
    disabled: null,
    password: validatePassword(f.password, false),
  });
}

export interface AliasForm {
  localPart: string;
  domain: string;
  displayName: string;
  shareSent: boolean;
  members: { user_id: number; can_send_as: boolean }[];
}

export function validateAlias(f: AliasForm): FieldErrors<AliasForm> {
  return compact<AliasForm>({
    localPart: validateLocalPart(f.localPart),
    domain: f.domain ? null : t('admin.validation.domainRequired'),
    displayName: validateName(f.displayName, false),
    shareSent: null,
    members: null,
  });
}

/** "local@domain" → [local, domain]. */
export function splitEmail(email: string): [string, string] {
  const at = email.lastIndexOf('@');
  return at < 0 ? [email, ''] : [email.slice(0, at), email.slice(at + 1)];
}

export const hasFieldErrors = (e: object): boolean => Object.values(e).some(Boolean);
