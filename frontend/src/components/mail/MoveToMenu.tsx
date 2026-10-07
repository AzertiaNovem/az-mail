/**
 * "移至" menu [WP-E]: 收件箱 / 垃圾邮件 / 已删除, then labels (moving to a label = add the label
 * and archive, like Gmail). The current location is left out.
 */
import { useMemo, useState, type ReactElement } from 'react';
import type { FolderId, Label } from '@/api/types';
import { Icon, Popover } from '@/components/common';
import { t } from '@/i18n/zh';
import { filterLabels } from './LabelMenu';
import { useLabels } from './queries';

export type MoveTarget = { kind: 'inbox' } | { kind: 'spam' } | { kind: 'trash' } | { kind: 'label'; label: Label };

export interface MoveToMenuProps {
  trigger: ReactElement;
  /** Current folder (its own entry is hidden). */
  folder: FolderId | null;
  currentLabelId: number | null;
  onMove: (target: MoveTarget) => void;
}

const itemClass =
  'flex h-8 w-full items-center gap-3 px-4 text-left text-sm hover:bg-on-surface/8 focus-visible:bg-on-surface/12 focus-visible:outline-none';

export function MoveToMenu({ trigger, folder, currentLabelId, onMove }: MoveToMenuProps) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const labels = useLabels().data;
  const shown = useMemo(() => filterLabels(labels ?? [], query).filter((l) => l.id !== currentLabelId), [labels, query, currentLabelId]);
  const pick = (target: MoveTarget) => {
    setOpen(false);
    setQuery('');
    onMove(target);
  };
  const q = query.trim().toLowerCase();
  const systems = (
    [
      { kind: 'inbox', icon: 'inbox', name: t('mail.move.inbox') },
      { kind: 'spam', icon: 'report', name: t('mail.move.spam') },
      { kind: 'trash', icon: 'delete', name: t('mail.move.trash') },
    ] as const
  ).filter((s) => s.kind !== folder && (!q || s.name.toLowerCase().includes(q)));

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
      aria-label={t('mail.move.title')}
    >
      <div className="px-4 pb-2 text-sm text-on-surface">{t('mail.move.title')}</div>
      <div className="mx-3 mb-1 flex h-9 items-center gap-2 rounded border border-outline-variant px-2 focus-within:border-primary">
        <input
          autoFocus
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder={t('mail.move.filter')}
          aria-label={t('mail.move.filter')}
          className="min-w-0 flex-1 bg-transparent text-sm outline-none placeholder:text-muted"
        />
        <Icon name="search" size={18} className="text-on-surface-variant" />
      </div>
      <ul className="max-h-72 overflow-y-auto py-1">
        {systems.map((s) => (
          <li key={s.kind}>
            <button type="button" className={itemClass} onClick={() => pick({ kind: s.kind })}>
              <Icon name={s.icon} size={18} className="text-on-surface-variant" />
              <span className="truncate">{s.name}</span>
            </button>
          </li>
        ))}
        {systems.length > 0 && shown.length > 0 && <li className="my-1 h-px bg-divider" aria-hidden="true" />}
        {shown.map((l) => (
          <li key={l.id}>
            <button type="button" className={itemClass} onClick={() => pick({ kind: 'label', label: l })}>
              <span className="ml-1 mr-1 size-2.5 shrink-0 rounded-full" style={{ backgroundColor: l.color }} aria-hidden="true" />
              <span className="truncate">{l.name}</span>
            </button>
          </li>
        ))}
        {systems.length === 0 && shown.length === 0 && (
          <li className="px-4 py-2 text-sm text-on-surface-variant">{t('mail.labels.noMatch')}</li>
        )}
      </ul>
    </Popover>
  );
}
