/**
 * Thread view toolbar [WP-E]: 返回, folder-aware 归档 / 举报垃圾邮件 / 删除 (恢复 / 永久删除 /
 * 不是垃圾邮件 in trash and spam), 标为未读, 移至, 标签, 更多, and "第 3 封，共 50 封" with
 * 较新 / 较旧 on the right.
 */
import type { Label, ThreadAction } from '@/api/types';
import { DropdownMenu, IconButton, type MenuEntry } from '@/components/common';
import { t } from '@/i18n/zh';
import { formatCount } from '@/lib/format';
import { LabelMenu, type LabelTarget } from './LabelMenu';
import { MoveToMenu, type MoveTarget } from './MoveToMenu';
import type { ListView } from './view';

export interface ThreadPosition {
  /** 1-based position in the whole list. */
  index: number;
  total: number | null;
}

export interface ThreadToolbarProps {
  view: ListView;
  target: LabelTarget;
  backLabel: string;
  inInbox: boolean;
  starred: boolean;
  position: ThreadPosition | null;
  hasNewer: boolean;
  hasOlder: boolean;
  onBack: () => void;
  onAction: (action: ThreadAction) => void;
  onMove: (target: MoveTarget) => void;
  onLabelToggle: (label: Label, add: boolean) => void;
  onNewer: () => void;
  onOlder: () => void;
}

export function ThreadToolbar(props: ThreadToolbarProps) {
  const { view } = props;
  const folder = view.folder;
  const btn = (action: ThreadAction, icon: string, label: string, shortcut?: string) => (
    <IconButton key={action} icon={icon} label={label} shortcut={shortcut} onClick={() => props.onAction(action)} />
  );
  let primary;
  if (folder === 'trash') primary = [btn('restore', 'restore_from_trash', t('mail.actions.restore')), btn('delete_forever', 'delete_forever', t('mail.actions.deleteForever'), '#')];
  else if (folder === 'spam')
    primary = [btn('not_spam', 'report_off', t('mail.actions.notSpam'), '!'), btn('delete_forever', 'delete_forever', t('mail.actions.deleteForever'), '#')];
  else
    primary = [
      props.inInbox ? btn('archive', 'archive', t('mail.actions.archive'), 'e') : btn('inbox', 'move_to_inbox', t('mail.actions.moveToInbox')),
      btn('spam', 'report', t('mail.actions.reportSpam'), '!'),
      btn('trash', 'delete', t('mail.actions.delete'), '#'),
    ];

  const more: MenuEntry[] = [
    props.starred
      ? { key: 'unstar', label: t('mail.actions.unstar'), icon: 'star', shortcut: 's', onSelect: () => props.onAction('unstar') }
      : { key: 'star', label: t('mail.actions.star'), icon: 'star', shortcut: 's', onSelect: () => props.onAction('star') },
    { key: 'read', label: t('mail.actions.markRead'), icon: 'drafts', shortcut: 'Shift+I', onSelect: () => props.onAction('read') },
  ];

  return (
    <div className="azm-toolbar flex h-12 shrink-0 items-center gap-0.5 overflow-x-auto px-2 sm:px-4" role="toolbar" aria-label={props.backLabel}>
      <IconButton icon="arrow_back" label={props.backLabel} shortcut="u" onClick={props.onBack} />
      <span className="mx-1 h-5 w-px bg-divider" aria-hidden="true" />
      {primary}
      <span className="mx-1 h-5 w-px bg-divider" aria-hidden="true" />
      {btn('unread', 'mark_email_unread', t('mail.actions.markUnread'), 'Shift+U')}
      {folder !== 'trash' && (
        <MoveToMenu
          folder={folder}
          currentLabelId={view.labelId}
          onMove={props.onMove}
          trigger={<IconButton icon="drive_file_move" label={t('mail.actions.moveTo')} />}
        />
      )}
      <LabelMenu targets={[props.target]} onToggle={props.onLabelToggle} trigger={<IconButton icon="label" label={t('mail.actions.labels')} />} />
      <DropdownMenu items={more} trigger={<IconButton icon="more_vert" label={t('mail.actions.more')} />} />
      <div className="ml-auto hidden shrink-0 items-center gap-1 text-xs text-on-surface-variant sm:flex">
        {props.position && (
          <span className="hidden whitespace-nowrap px-2 sm:inline">
            {props.position.total !== null
              ? t('mail.thread.position', { index: formatCount(props.position.index), total: formatCount(props.position.total) })
              : formatCount(props.position.index)}
          </span>
        )}
        <IconButton icon="chevron_left" label={t('mail.thread.newer')} shortcut="k" size="sm" disabled={!props.hasNewer} onClick={props.onNewer} />
        <IconButton icon="chevron_right" label={t('mail.thread.older')} shortcut="j" size="sm" disabled={!props.hasOlder} onClick={props.onOlder} />
      </div>
    </div>
  );
}
