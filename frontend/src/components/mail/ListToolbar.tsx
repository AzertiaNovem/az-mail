/**
 * Thread-list toolbar [WP-E]: select checkbox + 选择 menu, refresh / more when nothing is
 * selected, folder-aware bulk actions when something is, and the pager on the right.
 */
import type { ReactNode } from 'react';
import type { FolderId, Label, ThreadAction, ThreadListItem } from '@/api/types';
import { Checkbox, cx, DropdownMenu, IconButton, Spinner, type MenuEntry } from '@/components/common';
import { t } from '@/i18n/zh';
import { LabelMenu } from './LabelMenu';
import { MoveToMenu, type MoveTarget } from './MoveToMenu';
import { Pager, type PagerProps } from './Pager';

export type SelectKind = 'all' | 'none' | 'read' | 'unread' | 'starred' | 'unstarred';

/** Ids of `items` matching a 选择 menu entry. */
export function selectIds(items: readonly ThreadListItem[], kind: SelectKind): number[] {
  switch (kind) {
    case 'all':
      return items.map((i) => i.id);
    case 'none':
      return [];
    case 'read':
      return items.filter((i) => !i.unread).map((i) => i.id);
    case 'unread':
      return items.filter((i) => i.unread).map((i) => i.id);
    case 'starred':
      return items.filter((i) => i.starred).map((i) => i.id);
    case 'unstarred':
      return items.filter((i) => !i.starred).map((i) => i.id);
  }
}

export interface ListToolbarProps {
  items: readonly ThreadListItem[];
  selectedIds: readonly number[];
  folder: FolderId | null;
  currentLabelId: number | null;
  fetching: boolean;
  onSelect: (kind: SelectKind) => void;
  onRefresh: () => void;
  onAction: (action: ThreadAction) => void;
  onMove: (target: MoveTarget) => void;
  onLabelToggle: (label: Label, add: boolean) => void;
  onMarkAllRead: () => void;
  pager: PagerProps;
}

export function ListToolbar(props: ListToolbarProps) {
  const { items, selectedIds, folder, fetching } = props;
  const selected = items.filter((i) => selectedIds.includes(i.id));
  const n = selected.length;
  const allSelected = items.length > 0 && n === items.length;
  const master: boolean | 'indeterminate' = allSelected ? true : n > 0 ? 'indeterminate' : false;
  const anyUnread = selected.some((i) => i.unread);
  const anyInInbox = selected.some((i) => i.in_inbox);
  const anyStarred = selected.some((i) => i.starred);

  const selectEntries: MenuEntry[] = (
    [
      ['all', 'mail.list.selectAll'],
      ['none', 'mail.list.selectNone'],
      ['read', 'mail.list.selectRead'],
      ['unread', 'mail.list.selectUnread'],
      ['starred', 'mail.list.selectStarred'],
      ['unstarred', 'mail.list.selectUnstarred'],
    ] as const
  ).map(([kind, key]) => ({ key: kind, label: t(key), onSelect: () => props.onSelect(kind) }));

  const btn = (action: ThreadAction, icon: string, label: string, shortcut?: string) => (
    <IconButton key={action} icon={icon} label={label} shortcut={shortcut} onClick={() => props.onAction(action)} />
  );

  let actions: ReactNode = null;
  if (n > 0) {
    const readToggle = anyUnread ? btn('read', 'drafts', t('mail.actions.markRead'), 'Shift+I') : btn('unread', 'mark_email_unread', t('mail.actions.markUnread'), 'Shift+U');
    const move = (
      <MoveToMenu
        key="move"
        folder={folder}
        currentLabelId={props.currentLabelId}
        onMove={props.onMove}
        trigger={<IconButton icon="drive_file_move" label={t('mail.actions.moveTo')} />}
      />
    );
    const labels = (
      <LabelMenu
        key="labels"
        targets={selected}
        onToggle={props.onLabelToggle}
        trigger={<IconButton icon="label" label={t('mail.actions.labels')} />}
      />
    );
    const more: MenuEntry[] = [
      anyStarred
        ? { key: 'unstar', label: t('mail.actions.unstar'), icon: 'star', onSelect: () => props.onAction('unstar'), shortcut: 's' }
        : { key: 'star', label: t('mail.actions.star'), icon: 'star', onSelect: () => props.onAction('star'), shortcut: 's' },
      anyUnread
        ? { key: 'unread', label: t('mail.actions.markUnread'), icon: 'mark_email_unread', onSelect: () => props.onAction('unread') }
        : { key: 'read', label: t('mail.actions.markRead'), icon: 'drafts', onSelect: () => props.onAction('read') },
    ];
    const moreMenu = (
      <DropdownMenu key="more" items={more} trigger={<IconButton icon="more_vert" label={t('mail.actions.more')} />} />
    );
    const sep = <span key="sep" className="mx-1 h-5 w-px bg-divider" aria-hidden="true" />;
    if (folder === 'trash') {
      actions = [
        btn('restore', 'restore_from_trash', t('mail.actions.restore')),
        btn('delete_forever', 'delete_forever', t('mail.actions.deleteForever')),
        sep,
        readToggle,
        labels,
        moreMenu,
      ];
    } else if (folder === 'spam') {
      actions = [
        btn('not_spam', 'report_off', t('mail.actions.notSpam')),
        btn('delete_forever', 'delete_forever', t('mail.actions.deleteForever')),
        sep,
        readToggle,
        move,
        labels,
        moreMenu,
      ];
    } else {
      actions = [
        anyInInbox || folder === 'inbox'
          ? btn('archive', 'archive', t('mail.actions.archive'), 'e')
          : folder === 'drafts' || folder === 'scheduled'
            ? null
            : btn('inbox', 'move_to_inbox', t('mail.actions.moveToInbox')),
        btn('spam', 'report', t('mail.actions.reportSpam'), '!'),
        btn('trash', 'delete', t('mail.actions.delete'), '#'),
        sep,
        readToggle,
        move,
        labels,
        moreMenu,
      ];
    }
  }

  return (
    <div className="flex h-12 shrink-0 items-center gap-0.5 border-b border-transparent pl-2 pr-2 sm:pl-4" role="toolbar" aria-label={t('mail.list.label')}>
      <div className="flex items-center rounded hover:bg-on-surface/8">
        <span className="flex size-8 items-center justify-center">
          <Checkbox
            checked={master}
            aria-label={t('mail.list.select')}
            onCheckedChange={() => props.onSelect(n > 0 ? 'none' : 'all')}
          />
        </span>
        <DropdownMenu
          items={selectEntries}
          aria-label={t('mail.list.selectMenu')}
          trigger={<IconButton icon="arrow_drop_down" label={t('mail.list.selectMenu')} size="sm" className="-ml-2 w-5" tooltip={false} />}
        />
      </div>
      {n === 0 ? (
        <>
          <IconButton
            icon="refresh"
            label={t('mail.list.refresh')}
            onClick={props.onRefresh}
            className={cx(fetching && 'text-primary')}
          />
          <DropdownMenu
            items={[{ key: 'all-read', label: t('mail.list.markAllRead'), icon: 'drafts', onSelect: props.onMarkAllRead, disabled: !items.some((i) => i.unread) }]}
            trigger={<IconButton icon="more_vert" label={t('mail.list.more')} />}
          />
          {fetching && <Spinner size={18} className="ml-2" />}
        </>
      ) : (
        <>
          {actions}
          <span className="ml-2 hidden text-xs text-on-surface-variant md:inline">{t('mail.list.selected', { count: n })}</span>
        </>
      )}
      <div className="ml-auto">
        <Pager {...props.pager} />
      </div>
    </div>
  );
}
