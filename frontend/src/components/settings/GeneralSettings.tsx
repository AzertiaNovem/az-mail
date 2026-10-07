/**
 * Settings → 常规 [WP-F]: 显示名称, 撤销发送, 每页显示, 外部图片, 时区, 签名 (+ 启用),
 * 已信任图片发件人. Edits are local until 保存更改 (partial PUT /api/settings with the changed
 * fields); 取消 restores the saved values. Leaving the page with unsaved edits asks first.
 */
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { useId, useState, type ReactNode } from 'react';
import { useBlocker } from 'react-router';
import { errorMessage } from '@/api/client';
import { updateSettings } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Me, Settings } from '@/api/types';
import { ConfirmDialog, cx, FieldMessage, IconButton, Select, Switch, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { toast } from '@/stores/toast';
import { SaveBar } from './SaveBar';
import { SignatureEditor } from './SignatureEditor';
import { timezoneOptions } from './timezones';
import {
  DISPLAY_NAME_MAX,
  diffGeneral,
  generalFormFrom,
  hasErrors,
  PAGE_SIZE_OPTIONS,
  UNDO_SEND_OPTIONS,
  validateGeneral,
  type GeneralErrors,
  type GeneralForm,
} from './validation';

export function SettingsRow({ label, help, htmlFor, children }: { label: string; help?: string; htmlFor?: string; children: ReactNode }) {
  return (
    <div className="azm-settings-row">
      <div>
        {htmlFor ? (
          <label htmlFor={htmlFor} className="azm-settings-label">
            {label}
          </label>
        ) : (
          <div className="azm-settings-label">{label}</div>
        )}
        {help && <p className="azm-settings-help">{help}</p>}
      </div>
      <div className="min-w-0">{children}</div>
    </div>
  );
}

function sameForm(a: GeneralForm, b: GeneralForm): boolean {
  return Object.keys(diffGeneral(a, b)).length === 0;
}

export function GeneralSettings({ me }: { me: Me }) {
  const qc = useQueryClient();
  const ids = { name: useId(), undo: useId(), size: useId(), tz: useId() };
  const [baseline, setBaseline] = useState(() => generalFormFrom(me.settings));
  const [form, setForm] = useState(baseline);
  const [errors, setErrors] = useState<GeneralErrors>({});
  const [editorKey, setEditorKey] = useState(0);
  const [seen, setSeen] = useState(me.settings);

  const patch = diffGeneral(baseline, form);
  const dirty = Object.keys(patch).length > 0;

  // Settings changed elsewhere (another tab, settings.changed): adopt them unless editing.
  if (me.settings !== seen) {
    setSeen(me.settings);
    const next = generalFormFrom(me.settings);
    if (!dirty && !sameForm(next, baseline)) {
      setBaseline(next);
      setForm(next);
      setEditorKey((k) => k + 1);
    }
  }

  const mutation = useMutation({
    mutationFn: (p: Partial<GeneralForm>) => updateSettings(p),
    onSuccess: (s: Settings) => {
      qc.setQueryData<Me>(queryKeys.me(), (old) => (old ? { ...old, display_name: s.display_name, settings: s } : old));
      const next = generalFormFrom(s);
      setBaseline(next);
      setForm(next);
      setSeen(s);
      toast.push({ message: t('settings.saved') });
    },
    onError: (e) => toast.error(t('settings.saveFailed', { reason: errorMessage(e) })),
  });

  const set = <K extends keyof GeneralForm>(key: K, value: GeneralForm[K]) => {
    setForm((f) => ({ ...f, [key]: value }));
    if (errors[key]) setErrors((e) => ({ ...e, [key]: undefined }));
  };

  const save = () => {
    const errs = validateGeneral(form);
    setErrors(errs);
    if (hasErrors(errs) || !dirty) return;
    mutation.mutate(patch);
  };

  const cancel = () => {
    setForm(baseline);
    setErrors({});
    setEditorKey((k) => k + 1);
  };

  const blocker = useBlocker(({ currentLocation, nextLocation }) => dirty && currentLocation.pathname !== nextLocation.pathname);

  const undoOptions = UNDO_SEND_OPTIONS.map((n) => ({
    value: String(n),
    label: n === 0 ? t('settings.general.undoOff') : t('settings.general.undoSeconds', { n }),
  }));
  const sizes: number[] = [...PAGE_SIZE_OPTIONS];
  if (!sizes.includes(form.page_size)) sizes.push(form.page_size);
  const sizeOptions = sizes.sort((a, b) => a - b).map((n) => ({ value: String(n), label: t('settings.general.pageSizeOption', { n }) }));
  const tzOptions = timezoneOptions(form.timezone);

  return (
    <>
      <div className="azm-page-body">
        <form
          className="max-w-4xl"
          noValidate
          onSubmit={(e) => {
            e.preventDefault();
            save();
          }}
        >
          <SettingsRow label={t('settings.general.displayName')} help={t('settings.general.displayNameHelp')} htmlFor={ids.name}>
            <TextField
              id={ids.name}
              aria-label={t('settings.general.displayName')}
              containerClassName="max-w-sm"
              value={form.display_name}
              maxLength={DISPLAY_NAME_MAX + 20}
              error={errors.display_name}
              onChange={(e) => set('display_name', e.target.value)}
            />
          </SettingsRow>

          <SettingsRow label={t('settings.general.undoSend')} help={t('settings.general.undoSendHelp')} htmlFor={ids.undo}>
            <Select
              id={ids.undo}
              containerClassName="max-w-[12rem]"
              options={undoOptions}
              value={String(form.undo_send_seconds)}
              error={errors.undo_send_seconds}
              onValueChange={(v) => set('undo_send_seconds', Number(v) as GeneralForm['undo_send_seconds'])}
            />
          </SettingsRow>

          <SettingsRow label={t('settings.general.pageSize')} htmlFor={ids.size}>
            <Select
              id={ids.size}
              containerClassName="max-w-[12rem]"
              options={sizeOptions}
              value={String(form.page_size)}
              error={errors.page_size}
              onValueChange={(v) => set('page_size', Number(v))}
            />
          </SettingsRow>

          <SettingsRow label={t('settings.general.images')}>
            <div role="radiogroup" aria-label={t('settings.general.images')} className="flex flex-col gap-3">
              {(['ask', 'always'] as const).map((v) => (
                <label key={v} className="inline-flex cursor-pointer items-center gap-3 text-sm">
                  <input
                    type="radio"
                    name="remote_images"
                    className="size-4 accent-primary"
                    checked={form.remote_images === v}
                    onChange={() => set('remote_images', v)}
                  />
                  {v === 'ask' ? t('settings.general.imagesAsk') : t('settings.general.imagesAlways')}
                </label>
              ))}
            </div>
          </SettingsRow>

          <SettingsRow label={t('settings.general.timezone')} help={t('settings.general.timezoneHelp')} htmlFor={ids.tz}>
            <Select
              id={ids.tz}
              containerClassName="max-w-sm"
              options={tzOptions}
              value={form.timezone}
              error={errors.timezone}
              onValueChange={(v) => set('timezone', v)}
            />
          </SettingsRow>

          <SettingsRow label={t('settings.general.signature')} help={t('settings.general.signatureHelp')}>
            <div className="flex max-w-2xl flex-col gap-3">
              <Switch
                checked={form.signature_enabled}
                onCheckedChange={(v) => set('signature_enabled', v)}
                label={t('settings.general.signatureEnabled')}
              />
              <SignatureEditor
                key={editorKey}
                initialHtml={form.signature_html}
                invalid={!!errors.signature_html}
                onChange={(html) => set('signature_html', html)}
              />
              {errors.signature_html && (
                <FieldMessage id="signature-error" error>
                  {errors.signature_html}
                </FieldMessage>
              )}
            </div>
          </SettingsRow>

          <SettingsRow label={t('settings.general.trusted')} help={t('settings.general.trustedHelp')}>
            {form.trusted_image_senders.length === 0 ? (
              <p className="text-sm text-on-surface-variant">{t('settings.general.trustedEmpty')}</p>
            ) : (
              <ul className="m-0 flex max-w-md list-none flex-col p-0">
                {form.trusted_image_senders.map((email) => (
                  <li key={email} className={cx('flex h-10 items-center gap-2 border-b border-divider text-sm last:border-b-0')}>
                    <span className="min-w-0 flex-1 truncate">{email}</span>
                    <IconButton
                      icon="close"
                      size="sm"
                      label={t('settings.general.trustedRemove', { email })}
                      onClick={() =>
                        set(
                          'trusted_image_senders',
                          form.trusted_image_senders.filter((x) => x !== email),
                        )
                      }
                    />
                  </li>
                ))}
              </ul>
            )}
          </SettingsRow>
          <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
        </form>
      </div>
      <SaveBar dirty={dirty} saving={mutation.isPending} onSave={save} onCancel={cancel} />
      <ConfirmDialog
        open={blocker.state === 'blocked'}
        onOpenChange={(o) => {
          if (!o && blocker.state === 'blocked') blocker.reset();
        }}
        title={t('settings.leave.title')}
        message={t('settings.leave.message')}
        confirmLabel={t('settings.leave.confirm')}
        cancelLabel={t('settings.leave.cancel')}
        danger
        onConfirm={() => blocker.state === 'blocked' && blocker.proceed()}
      />
    </>
  );
}
