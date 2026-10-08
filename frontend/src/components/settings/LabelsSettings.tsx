/**
 * Settings → 标签 [WP-F]: label table (color swatch picker, name, thread count, 修改 / 删除) and
 * 新建标签. Mutations invalidate labels, counts and thread lists (labels show on rows).
 */
import { useMutation, useQuery, useQueryClient, type QueryClient } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { errorMessage, isApiError } from '@/api/client';
import { createLabel, deleteLabel, getCounts, listLabels, updateLabel } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import type { Label, LabelInput } from '@/api/types';
import { Button, ConfirmDialog, cx, Dialog, IconButton, Popover, Spinner, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { LABEL_NAME_INPUT_MAX } from '@/lib/labelName';
import { toast } from '@/stores/toast';
import { LABEL_NAME_MAX, validateLabelName } from './validation';

/** Gmail-like label colors; DEFAULT_LABEL_COLOR is the schema default. */
export const DEFAULT_LABEL_COLOR = '#9aa0a6';
export const LABEL_COLORS = [
  '#9aa0a6', '#000000', '#434343', '#666666', '#999999', '#cccccc', '#e7e7e7', '#f3f3f3',
  '#fb4c2f', '#ffad47', '#fad165', '#16a766', '#43d692', '#4a86e8', '#a479e2', '#f691b3',
  '#cc3a21', '#eaa041', '#f2c960', '#149e60', '#3dc789', '#3c78d8', '#8e63ce', '#e07798',
  '#ac2b16', '#cf8933', '#d5ae49', '#0b804b', '#2a9c68', '#285bac', '#653e9b', '#b65775',
] as const;

export function ColorSwatches({ value, onChange }: { value: string; onChange: (color: string) => void }) {
  const current = value.toLowerCase();
  return (
    <div role="radiogroup" aria-label={t('settings.labels.colorLabel')} className="grid grid-cols-8 gap-1.5">
      {LABEL_COLORS.map((c) => (
        <button
          key={c}
          type="button"
          role="radio"
          aria-checked={current === c}
          aria-label={t('settings.labels.chooseColor', { color: c })}
          title={c}
          onClick={() => onChange(c)}
          className={cx(
            'inline-flex size-6 items-center justify-center rounded-full border border-black/10 transition-transform hover:scale-110',
            current === c && 'ring-2 ring-primary ring-offset-2',
          )}
          style={{ backgroundColor: c }}
        />
      ))}
    </div>
  );
}

function invalidateLabels(qc: QueryClient) {
  void qc.invalidateQueries({ queryKey: queryKeys.labels() });
  void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
}

interface LabelDialogProps {
  label: Label | null; // null = create
  onClose: () => void;
}

function LabelDialog({ label, onClose }: LabelDialogProps) {
  const qc = useQueryClient();
  const [name, setName] = useState(label?.name ?? '');
  const [color, setColor] = useState(label?.color ?? DEFAULT_LABEL_COLOR);
  const [error, setError] = useState<string | null>(null);
  const mutation = useMutation({
    mutationFn: (input: LabelInput) => (label ? updateLabel(label.id, input) : createLabel(input)),
    onSuccess: (saved) => {
      invalidateLabels(qc);
      toast.push({ message: label ? t('settings.labels.updated') : t('settings.labels.created', { name: saved.name }) });
      onClose();
    },
    // label_exists comes with a Chinese message; an invalid name gets the length hint. Both go
    // under the name field.
    onError: (e) =>
      setError(
        isApiError(e, 'invalid_field') && e.details.field === 'name'
          ? t('settings.labels.nameTooLong', { max: LABEL_NAME_MAX })
          : errorMessage(e),
      ),
  });
  const submit = (e?: FormEvent) => {
    e?.preventDefault();
    const err = validateLabelName(name);
    setError(err);
    if (err) return;
    mutation.mutate({ name: name.trim(), color });
  };
  return (
    <Dialog
      open
      onOpenChange={(o) => !o && !mutation.isPending && onClose()}
      title={label ? t('settings.labels.editTitle') : t('settings.labels.createTitle')}
      size="sm"
      dismissible={!mutation.isPending}
      footer={
        <>
          <Button variant="text" onClick={onClose} disabled={mutation.isPending}>
            {t('actions.cancel')}
          </Button>
          <Button onClick={() => submit()} loading={mutation.isPending}>
            {label ? t('actions.save') : t('actions.create')}
          </Button>
        </>
      }
    >
      <form className="flex flex-col gap-5" onSubmit={submit} noValidate>
        <TextField
          label={t('settings.labels.name')}
          value={name}
          autoFocus
          maxLength={LABEL_NAME_INPUT_MAX}
          error={error ?? undefined}
          onChange={(e) => {
            setName(e.target.value);
            setError(null);
          }}
        />
        <div className="flex flex-col gap-2">
          <span className="text-sm font-medium text-on-surface-variant">{t('settings.labels.color')}</span>
          <ColorSwatches value={color} onChange={setColor} />
        </div>
        <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
      </form>
    </Dialog>
  );
}

function ColorCell({ label }: { label: Label }) {
  const qc = useQueryClient();
  const [open, setOpen] = useState(false);
  const mutation = useMutation({
    mutationFn: (color: string) => updateLabel(label.id, { color }),
    onSuccess: () => invalidateLabels(qc),
    onError: (e) => toast.error(errorMessage(e)),
  });
  return (
    <Popover
      open={open}
      onOpenChange={setOpen}
      aria-label={t('settings.labels.changeColor', { name: label.name })}
      trigger={
        <button
          type="button"
          aria-label={t('settings.labels.changeColor', { name: label.name })}
          className="inline-flex size-6 items-center justify-center rounded-full border border-black/10 transition-transform hover:scale-110"
          style={{ backgroundColor: label.color }}
        />
      }
    >
      <ColorSwatches
        value={label.color}
        onChange={(c) => {
          setOpen(false);
          if (c !== label.color) mutation.mutate(c);
        }}
      />
    </Popover>
  );
}

export function LabelsSettings() {
  const qc = useQueryClient();
  const labels = useQuery({ queryKey: queryKeys.labels(), queryFn: ({ signal }) => listLabels(signal), staleTime: staleTimes.labels });
  const counts = useQuery({ queryKey: queryKeys.counts(), queryFn: ({ signal }) => getCounts(signal), staleTime: staleTimes.counts });
  const [editing, setEditing] = useState<Label | null | 'new'>(null);
  const [deleting, setDeleting] = useState<Label | null>(null);
  const del = useMutation({
    mutationFn: (l: Label) => deleteLabel(l.id),
    onSuccess: (_v, l) => {
      invalidateLabels(qc);
      toast.push({ message: t('settings.labels.deleted', { name: l.name }) });
      setDeleting(null);
    },
    onError: (e) => {
      toast.error(errorMessage(e));
      setDeleting(null);
    },
  });

  const rows = [...(labels.data ?? [])].sort((a, b) => a.sort_order - b.sort_order || a.name.localeCompare(b.name, 'zh-CN'));

  return (
    <div className="azm-page-body">
      <div className="flex max-w-4xl flex-col gap-4">
        <div className="flex flex-wrap items-center gap-3">
          <p className="m-0 flex-1 text-sm text-on-surface-variant">{t('settings.labels.help')}</p>
          <Button icon="add" onClick={() => setEditing('new')}>
            {t('settings.labels.create')}
          </Button>
        </div>
        {labels.isPending ? (
          <div className="flex justify-center py-10">
            <Spinner />
          </div>
        ) : labels.isError ? (
          <div className="azm-banner tone-error">
            <span className="flex-1">{t('settings.loadFailed', { reason: errorMessage(labels.error) })}</span>
            <Button variant="text" size="sm" onClick={() => void labels.refetch()}>
              {t('actions.retry')}
            </Button>
          </div>
        ) : rows.length === 0 ? (
          <p className="py-10 text-center text-sm text-on-surface-variant">{t('settings.labels.empty')}</p>
        ) : (
          <div className="azm-table-wrap">
            <table className="azm-table">
              <thead>
                <tr>
                  <th className="w-16">{t('settings.labels.color')}</th>
                  <th>{t('settings.labels.name')}</th>
                  <th className="num w-28">{t('settings.labels.threads')}</th>
                  <th className="w-28 text-right">{t('settings.labels.actions')}</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((l) => {
                  const c = counts.data?.labels[String(l.id)];
                  return (
                    <tr key={l.id}>
                      <td>
                        <ColorCell label={l} />
                      </td>
                      <td className="max-w-0 truncate font-medium">{l.name}</td>
                      <td className="num text-on-surface-variant">{c ? t('settings.labels.threadCount', { count: c.total }) : ''}</td>
                      <td className="text-right whitespace-nowrap">
                        <IconButton icon="edit" size="sm" label={`${t('settings.labels.rename')} ${l.name}`} onClick={() => setEditing(l)} />
                        <IconButton icon="delete" size="sm" label={`${t('settings.labels.delete')} ${l.name}`} onClick={() => setDeleting(l)} />
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </div>
      {editing !== null && <LabelDialog label={editing === 'new' ? null : editing} onClose={() => setEditing(null)} />}
      <ConfirmDialog
        open={deleting !== null}
        onOpenChange={(o) => !o && !del.isPending && setDeleting(null)}
        title={t('settings.labels.deleteTitle', { name: deleting?.name ?? '' })}
        message={t('settings.labels.deleteMessage')}
        confirmLabel={t('settings.labels.delete')}
        danger
        busy={del.isPending}
        onConfirm={() => deleting && del.mutate(deleting)}
      />
    </div>
  );
}
