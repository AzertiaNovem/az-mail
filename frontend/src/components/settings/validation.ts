/**
 * Settings form validation and diffing [WP-F] (pure, unit-tested). Messages are Chinese and
 * keyed by field so the forms can show them under the right control.
 */
import type { Settings } from '@/api/types';
import { t } from '@/i18n/zh';
import { isBlankHtml } from '@/lib/emailHtml';
import { LABEL_NAME_MAX, labelNameLength } from '@/lib/labelName';

export const DISPLAY_NAME_MAX = 100;
/** Server limit for signature_html (repo::SettingsPatch, ≤ 64 KiB). */
export const SIGNATURE_MAX_BYTES = 64 * 1024;
export const UNDO_SEND_OPTIONS = [0, 5, 10, 20, 30] as const;
export const PAGE_SIZE_OPTIONS = [10, 15, 20, 25, 50, 100] as const;
export const PAGE_SIZE_MIN = 10;
export const PAGE_SIZE_MAX = 100;
export const PASSWORD_MIN = 8;
/** Server limit (64 code points), shared with the mail-side label dialog. */
export { LABEL_NAME_MAX };

/** The editable part of Settings (everything the general tab shows). */
export type GeneralForm = Pick<
  Settings,
  | 'display_name'
  | 'undo_send_seconds'
  | 'page_size'
  | 'remote_images'
  | 'timezone'
  | 'signature_html'
  | 'signature_enabled'
  | 'trusted_image_senders'
>;

export type GeneralErrors = Partial<Record<keyof GeneralForm, string>>;

export function generalFormFrom(s: Settings): GeneralForm {
  return {
    display_name: s.display_name,
    undo_send_seconds: s.undo_send_seconds,
    page_size: s.page_size,
    remote_images: s.remote_images,
    timezone: s.timezone,
    signature_html: s.signature_html,
    signature_enabled: s.signature_enabled,
    trusted_image_senders: [...s.trusted_image_senders],
  };
}

function isValidTimeZone(tz: string): boolean {
  if (!tz) return false;
  try {
    new Intl.DateTimeFormat('en-US', { timeZone: tz });
    return true;
  } catch {
    return false;
  }
}

const utf8Length = (s: string): number => new TextEncoder().encode(s).length;

export function validateGeneral(form: GeneralForm): GeneralErrors {
  const errors: GeneralErrors = {};
  const name = form.display_name.trim();
  if (!name) errors.display_name = t('settings.validation.displayNameRequired');
  else if (Array.from(name).length > DISPLAY_NAME_MAX)
    errors.display_name = t('settings.validation.displayNameTooLong', { max: DISPLAY_NAME_MAX });
  if (!(UNDO_SEND_OPTIONS as readonly number[]).includes(form.undo_send_seconds)) errors.undo_send_seconds = t('settings.validation.undo');
  if (!Number.isInteger(form.page_size) || form.page_size < PAGE_SIZE_MIN || form.page_size > PAGE_SIZE_MAX)
    errors.page_size = t('settings.validation.pageSize');
  if (!isValidTimeZone(form.timezone)) errors.timezone = t('settings.validation.timezone');
  if (utf8Length(form.signature_html) > SIGNATURE_MAX_BYTES)
    errors.signature_html = t('settings.validation.signatureTooLong', { max: SIGNATURE_MAX_BYTES / 1024 });
  return errors;
}

/** Normalizes values before comparing / saving (trimmed name, '' for a blank signature). */
export function normalizeGeneral(form: GeneralForm): GeneralForm {
  return {
    ...form,
    display_name: form.display_name.trim(),
    signature_html: isBlankHtml(form.signature_html) ? '' : form.signature_html,
  };
}

/** Partial PUT /api/settings body with only the changed fields (empty object = no change). */
export function diffGeneral(initial: GeneralForm, form: GeneralForm): Partial<GeneralForm> {
  const a = normalizeGeneral(initial);
  const b = normalizeGeneral(form);
  const patch: Partial<GeneralForm> = {};
  for (const key of Object.keys(b) as (keyof GeneralForm)[]) {
    const changed = key === 'trusted_image_senders' ? a[key].join('\n') !== b[key].join('\n') : a[key] !== b[key];
    if (changed) (patch as Record<string, unknown>)[key] = b[key];
  }
  return patch;
}

// ───────────── password change ─────────────

export interface PasswordForm {
  current: string;
  next: string;
  confirm: string;
}

export type PasswordErrors = Partial<Record<keyof PasswordForm, string>>;

export function validatePasswordChange(f: PasswordForm): PasswordErrors {
  const errors: PasswordErrors = {};
  if (!f.current) errors.current = t('settings.account.currentRequired');
  if (!f.next) errors.next = t('settings.account.newRequired');
  else if (Array.from(f.next).length < PASSWORD_MIN) errors.next = t('settings.account.tooShort', { min: PASSWORD_MIN });
  else if (f.current && f.next === f.current) errors.next = t('settings.account.sameAsCurrent');
  if (!errors.next && f.confirm !== f.next) errors.confirm = t('settings.account.mismatch');
  return errors;
}

// ───────────── labels ─────────────

export function validateLabelName(name: string): string | null {
  const n = name.trim();
  if (!n) return t('settings.labels.nameRequired');
  if (labelNameLength(n) > LABEL_NAME_MAX) return t('settings.labels.nameTooLong', { max: LABEL_NAME_MAX });
  return null;
}

export const hasErrors = (e: object): boolean => Object.values(e).some(Boolean);
