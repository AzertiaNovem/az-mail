/**
 * Create / edit label dialog [WP-E]. One host is mounted by AppShell; open it from anywhere with
 * `openLabelDialog({mode:'create'})` (sidebar "+", the 标签 menu's 新建标签) or
 * `openLabelDialog({mode:'edit', label})` (label context menu).
 */
import { useQueryClient } from '@tanstack/react-query';
import { useId, useState, type FormEvent } from 'react';
import { create } from 'zustand';
import { errorMessage, isApiError } from '@/api/client';
import { createLabel, updateLabel } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Label } from '@/api/types';
import { Button, cx, Dialog, Icon, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { DEFAULT_LABEL_COLOR, LABEL_COLORS, readableTextColor } from '@/lib/color';
import { toast } from '@/stores/toast';

export const LABEL_NAME_MAX = 225;

export type LabelDialogRequest =
  | { mode: 'create'; initialName?: string; onSaved?: (label: Label) => void }
  | { mode: 'edit'; label: Label; onSaved?: (label: Label) => void };

interface LabelDialogState {
  request: LabelDialogRequest | null;
  /** Bumped on every open so the form starts fresh. */
  seq: number;
}

const useLabelDialogStore = create<LabelDialogState>(() => ({ request: null, seq: 0 }));

export function openLabelDialog(request: LabelDialogRequest): void {
  useLabelDialogStore.setState((s) => ({ request, seq: s.seq + 1 }));
}

export function closeLabelDialog(): void {
  useLabelDialogStore.setState({ request: null });
}

/** Writes a created / updated label into `['labels']` without waiting for the refetch. */
export function upsertCachedLabel(labels: Label[] | undefined, label: Label): Label[] {
  const list = labels ?? [];
  return list.some((l) => l.id === label.id) ? list.map((l) => (l.id === label.id ? label : l)) : [...list, label];
}

/** Palette picker (radio group of swatches). */
export function ColorSwatches({ value, onChange, labelledBy }: { value: string; onChange: (c: string) => void; labelledBy?: string }) {
  return (
    <div role="radiogroup" aria-labelledby={labelledBy} className="flex flex-wrap gap-2">
      {LABEL_COLORS.map((c) => {
        const selected = c.toLowerCase() === value.toLowerCase();
        return (
          <button
            key={c}
            type="button"
            role="radio"
            aria-checked={selected}
            aria-label={t('mail.labels.colorOption', { color: c })}
            onClick={() => onChange(c)}
            className={cx(
              'flex size-7 items-center justify-center rounded-full transition-transform hover:scale-110',
              selected && 'ring-2 ring-primary ring-offset-2',
            )}
            style={{ backgroundColor: c, color: readableTextColor(c) }}
          >
            {selected && <Icon name="check" size={16} weight={700} />}
          </button>
        );
      })}
    </div>
  );
}

function LabelForm({ request, onDone }: { request: LabelDialogRequest; onDone: () => void }) {
  const qc = useQueryClient();
  const editing = request.mode === 'edit' ? request.label : null;
  const [name, setName] = useState(editing ? editing.name : request.mode === 'create' ? (request.initialName ?? '') : '');
  const [color, setColor] = useState(editing?.color ?? DEFAULT_LABEL_COLOR);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const colorLabelId = useId();

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const trimmed = name.trim();
    if (!trimmed) {
      setError(t('mail.labels.nameRequired'));
      return;
    }
    if (trimmed.length > LABEL_NAME_MAX) {
      setError(t('mail.labels.nameTooLong', { max: LABEL_NAME_MAX }));
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const saved = editing ? await updateLabel(editing.id, { name: trimmed, color }) : await createLabel({ name: trimmed, color });
      qc.setQueryData<Label[]>(queryKeys.labels(), (old) => upsertCachedLabel(old, saved));
      void qc.invalidateQueries({ queryKey: queryKeys.labels() });
      void qc.invalidateQueries({ queryKey: queryKeys.counts() });
      toast.push({ message: t(editing ? 'mail.labels.updated' : 'mail.labels.created', { name: saved.name }) });
      request.onSaved?.(saved);
      onDone();
    } catch (err) {
      setError(isApiError(err, 'label_exists') ? t('errors.label_exists') : errorMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <form id="azm-label-form" onSubmit={(e) => void submit(e)} className="flex flex-col gap-5 pb-1">
      <TextField
        label={t('mail.labels.name')}
        placeholder={t('mail.labels.namePlaceholder')}
        value={name}
        maxLength={LABEL_NAME_MAX + 20}
        onChange={(e) => {
          setName(e.target.value);
          if (error) setError(null);
        }}
        error={error ?? undefined}
        autoFocus
        disabled={busy}
      />
      <div className="flex flex-col gap-2">
        <span id={colorLabelId} className="text-sm font-medium text-on-surface-variant">
          {t('mail.labels.color')}
        </span>
        <ColorSwatches value={color} onChange={setColor} labelledBy={colorLabelId} />
      </div>
      <div className="flex justify-end gap-2 pt-1">
        <Button variant="text" onClick={onDone} disabled={busy}>
          {t('actions.cancel')}
        </Button>
        <Button type="submit" loading={busy}>
          {editing ? t('mail.labels.save') : t('mail.labels.create')}
        </Button>
      </div>
    </form>
  );
}

/** Mounted once (AppShell). */
export function LabelDialogHost() {
  const request = useLabelDialogStore((s) => s.request);
  const seq = useLabelDialogStore((s) => s.seq);
  return (
    <Dialog
      open={request !== null}
      onOpenChange={(open) => {
        if (!open) closeLabelDialog();
      }}
      title={request?.mode === 'edit' ? t('mail.labels.editTitle') : t('mail.labels.createTitle')}
      size="sm"
    >
      {request && <LabelForm key={seq} request={request} onDone={closeLabelDialog} />}
    </Dialog>
  );
}
