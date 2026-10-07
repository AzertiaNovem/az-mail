/**
 * "标签" picker [WP-E]: a popover with a filter box and one tri-state checkbox per label
 * (checked = every selected thread has it, indeterminate = some do). Toggling applies at once
 * (add_label / remove_label, optimistic); "新建标签" opens the label dialog with the filter text.
 */
import { useMemo, useState, type ReactElement } from 'react';
import type { Label } from '@/api/types';
import { cx, Icon, Popover } from '@/components/common';
import { t } from '@/i18n/zh';
import { openLabelDialog } from './LabelDialog';
import { useLabels } from './queries';

export interface LabelTarget {
  id: number;
  label_ids: number[];
}

export type LabelState = boolean | 'indeterminate';

/** Checkbox state of `labelId` over the target threads. */
export function labelState(targets: readonly LabelTarget[], labelId: number): LabelState {
  if (targets.length === 0) return false;
  const n = targets.filter((x) => x.label_ids.includes(labelId)).length;
  return n === 0 ? false : n === targets.length ? true : 'indeterminate';
}

export function filterLabels(labels: readonly Label[], query: string): Label[] {
  const q = query.trim().toLowerCase();
  return q ? labels.filter((l) => l.name.toLowerCase().includes(q)) : [...labels];
}

export interface LabelMenuProps {
  trigger: ReactElement;
  targets: readonly LabelTarget[];
  /** add = true → add_label, false → remove_label. */
  onToggle: (label: Label, add: boolean) => void;
}

function MenuCheck({ state }: { state: LabelState }) {
  return (
    <span
      aria-hidden="true"
      className={cx(
        'inline-flex size-[18px] shrink-0 items-center justify-center rounded-[2px] border-2',
        state ? 'border-primary bg-primary text-on-primary' : 'border-on-surface-variant',
      )}
    >
      {state === 'indeterminate' ? <Icon name="remove" size={16} weight={700} /> : state ? <Icon name="check" size={16} weight={700} /> : null}
    </span>
  );
}

export function LabelMenu({ trigger, targets, onToggle }: LabelMenuProps) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const labels = useLabels().data;
  const shown = useMemo(() => filterLabels(labels ?? [], query), [labels, query]);
  const exact = (labels ?? []).some((l) => l.name.toLowerCase() === query.trim().toLowerCase());

  return (
    <Popover
      open={open}
      onOpenChange={(o) => {
        setOpen(o);
        if (!o) setQuery('');
      }}
      trigger={trigger}
      align="start"
      className="w-64 py-2"
      aria-label={t('mail.labels.apply')}
    >
      <div className="px-4 pb-2 text-sm text-on-surface">{t('mail.labels.apply')}</div>
      <div className="mx-3 mb-1 flex h-9 items-center gap-2 rounded border border-outline-variant px-2 focus-within:border-primary">
        <input
          autoFocus
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder={t('mail.labels.filter')}
          aria-label={t('mail.labels.filter')}
          className="min-w-0 flex-1 bg-transparent text-sm outline-none placeholder:text-muted"
        />
        <Icon name="search" size={18} className="text-on-surface-variant" />
      </div>
      <ul className="max-h-64 overflow-y-auto py-1" role="group" aria-label={t('mail.labels.apply')}>
        {shown.length === 0 && (
          <li className="px-4 py-2 text-sm text-on-surface-variant">
            {labels && labels.length ? t('mail.labels.noMatch') : t('mail.labels.none')}
          </li>
        )}
        {shown.map((l) => {
          const state = labelState(targets, l.id);
          return (
            <li key={l.id}>
              <button
                type="button"
                role="menuitemcheckbox"
                aria-checked={state === 'indeterminate' ? 'mixed' : state}
                onClick={() => onToggle(l, state !== true)}
                className="flex h-8 w-full items-center gap-3 px-4 text-left text-sm hover:bg-on-surface/8 focus-visible:bg-on-surface/12 focus-visible:outline-none"
              >
                <MenuCheck state={state} />
                <span className="size-2.5 shrink-0 rounded-full" style={{ backgroundColor: l.color }} aria-hidden="true" />
                <span className="min-w-0 flex-1 truncate">{l.name}</span>
              </button>
            </li>
          );
        })}
      </ul>
      <div className="my-1 h-px bg-divider" />
      <button
        type="button"
        onClick={() => {
          const name = exact ? '' : query.trim();
          setOpen(false);
          setQuery('');
          openLabelDialog({
            mode: 'create',
            initialName: name,
            onSaved: (label) => onToggle(label, true),
          });
        }}
        className="flex h-8 w-full items-center gap-3 px-4 text-left text-sm hover:bg-on-surface/8 focus-visible:bg-on-surface/12 focus-visible:outline-none"
      >
        <Icon name="add" size={18} className="text-on-surface-variant" />
        <span className="truncate">{query.trim() && !exact ? t('mail.labels.createNamed', { name: query.trim() }) : t('mail.labels.createNew')}</span>
      </button>
    </Popover>
  );
}
