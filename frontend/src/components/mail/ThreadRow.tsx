import { memo, type MouseEvent } from 'react';
import { Link } from 'react-router';
import type { FolderId, Label, ThreadAction, ThreadListItem } from '@/api/types';
import { Avatar, Checkbox, cx, Icon, IconButton } from '@/components/common';
import { statusName, t } from '@/i18n/zh';
import { formatFullDate, formatListDate, formatScheduleTime } from '@/lib/format';
import { displaySubject } from '@/lib/subject';
import type { Density } from '@/stores/ui';
import { fileIconFor } from './fileIcon';
import { LabelChip } from './LabelChip';
import { participantsView } from './participants';

export type ChipTone = 'neutral' | 'info' | 'warning' | 'error' | 'success';

export interface StatusChip {
  text: string;
  tone: ChipTone;
}

const SCHEDULED_STATES = new Set(['queued', 'sending', 'accepted', 'scheduled']);

/** Outbound status chip of a row (null for normal sent / delivered / inbound threads). */
export function rowStatusChip(item: ThreadListItem, folder: FolderId | null, now: number, tz: string): StatusChip | null {
  const s = item.latest_status;
  if (item.scheduled_at !== null && (s === null || SCHEDULED_STATES.has(s))) {
    if (folder === 'scheduled') return null; // the date column already shows the time
    return { text: statusName('scheduled', formatScheduleTime(item.scheduled_at, now, tz)), tone: 'info' };
  }
  switch (s) {
    case 'failed':
    case 'bounced':
    case 'suppressed':
    case 'complained':
      return { text: statusName(s), tone: 'error' };
    case 'delivery_delayed':
      return { text: statusName(s), tone: 'warning' };
    case 'queued':
    case 'sending':
      return { text: statusName(s), tone: 'neutral' };
    default:
      return null;
  }
}

export const CHIP_TONES: Record<ChipTone, string> = {
  neutral: 'bg-surface-container-highest text-on-surface-variant',
  info: 'bg-primary-container text-on-primary-container',
  warning: 'bg-warning-container text-warning',
  error: 'bg-error-container text-on-error-container',
  success: 'bg-success-container text-success',
};

const DENSITY_HEIGHT: Record<Density, string> = {
  compact: 'min-h-8',
  default: 'min-h-10',
  comfortable: 'min-h-12',
};

export interface ThreadRowProps {
  item: ThreadListItem;
  href: string;
  /** Current folder (null in label / search views). */
  folder: FolderId | null;
  /** Label of the current view; its chip is not repeated on rows. */
  currentLabelId: number | null;
  labelsById: ReadonlyMap<number, Label>;
  selected: boolean;
  focused: boolean;
  now: number;
  tz: string;
  mobile?: boolean;
  density?: Density;
  /** Stable callbacks (they receive the row item) so memoized rows do not re-render. */
  onOpen: (item: ThreadListItem, e: MouseEvent<HTMLAnchorElement>) => void;
  onSelect: (item: ThreadListItem, checked: boolean, shiftKey: boolean) => void;
  onStar: (item: ThreadListItem) => void;
  onAction: (item: ThreadListItem, action: ThreadAction) => void;
}

function rowActions(item: ThreadListItem, folder: FolderId | null): { action: ThreadAction; icon: string; label: string }[] {
  const out: { action: ThreadAction; icon: string; label: string }[] = [];
  if (folder === 'trash') {
    out.push({ action: 'restore', icon: 'restore_from_trash', label: t('mail.actions.restore') });
    out.push({ action: 'delete_forever', icon: 'delete_forever', label: t('mail.actions.deleteForever') });
  } else if (folder === 'spam') {
    out.push({ action: 'not_spam', icon: 'report_off', label: t('mail.actions.notSpam') });
    out.push({ action: 'delete_forever', icon: 'delete_forever', label: t('mail.actions.deleteForever') });
  } else {
    if (item.in_inbox) out.push({ action: 'archive', icon: 'archive', label: t('mail.actions.archive') });
    else if (folder !== 'drafts' && folder !== 'scheduled')
      out.push({ action: 'inbox', icon: 'move_to_inbox', label: t('mail.actions.moveToInbox') });
    out.push({ action: 'trash', icon: 'delete', label: t('mail.actions.delete') });
  }
  if (folder !== 'drafts')
    out.push(
      item.unread
        ? { action: 'read', icon: 'drafts', label: t('mail.actions.markRead') }
        : { action: 'unread', icon: 'mark_email_unread', label: t('mail.actions.markUnread') },
    );
  return out;
}

function Participants({ item, folder, className }: { item: ThreadListItem; folder: FolderId | null; className?: string }) {
  const view = participantsView(item, folder);
  return (
    <span className={cx('min-w-0 truncate', className)} title={view.title}>
      {view.prefix && <span className="font-normal">{view.prefix}</span>}
      {view.parts.map((p, i) => (
        <span key={i}>
          {i > 0 && !p.ellipsis && !view.parts[i - 1]?.ellipsis && <span className="font-normal">, </span>}
          {p.ellipsis ? (
            <span className="font-normal"> .. </span>
          ) : (
            <span className={cx(p.draft ? 'font-normal text-[#d93025]' : p.bold ? 'font-bold' : 'font-normal')}>{p.text}</span>
          )}
        </span>
      ))}
      {view.count !== null && <span className="ml-1 text-xs font-normal text-on-surface-variant">{view.count}</span>}
    </span>
  );
}

function ChipsAndSubject({
  item,
  folder,
  currentLabelId,
  labelsById,
  chip,
  showSnippet,
}: Pick<ThreadRowProps, 'item' | 'folder' | 'currentLabelId' | 'labelsById'> & { chip: StatusChip | null; showSnippet: boolean }) {
  const labels = item.label_ids
    .filter((id) => id !== currentLabelId)
    .map((id) => labelsById.get(id))
    .filter((l): l is Label => l !== undefined)
    .slice(0, 3);
  const showInbox = item.in_inbox && folder !== 'inbox' && (folder === null || folder === 'all' || folder === 'starred');
  return (
    <span className="flex min-w-0 items-center gap-1">
      {showInbox && <LabelChip name={t('mail.list.inbox')} />}
      {labels.map((l) => (
        <LabelChip key={l.id} name={l.name} color={l.color} />
      ))}
      {chip && (
        <span className={cx('inline-flex h-[18px] shrink-0 items-center rounded px-1 text-xs font-medium', CHIP_TONES[chip.tone])}>
          {chip.text}
        </span>
      )}
      <span className="min-w-0 truncate">
        <span className={item.unread ? 'font-bold text-on-surface' : 'font-normal text-on-surface'}>{displaySubject(item.subject)}</span>
        {showSnippet && item.snippet && <span className="font-normal text-on-surface-variant"> - {item.snippet}</span>}
      </span>
    </span>
  );
}

function AttachmentChips({ item }: { item: ThreadListItem }) {
  if (!item.attachments_preview.length) return null;
  return (
    <span className="mt-1 flex min-w-0 gap-2 overflow-hidden" aria-hidden="true">
      {item.attachments_preview.slice(0, 3).map((a) => {
        const fi = fileIconFor(a.content_type, a.filename);
        return (
          <span
            key={a.id}
            className="inline-flex h-7 max-w-[180px] shrink-0 items-center gap-1.5 rounded-full border border-outline-variant bg-surface-container px-2.5 text-xs font-normal text-on-surface-variant"
          >
            <Icon name={fi.icon} size={16} style={{ color: fi.color }} />
            <span className="truncate">{a.filename}</span>
          </span>
        );
      })}
    </span>
  );
}

/** One row of the thread list (Gmail layout; two-line card layout on mobile). */
export const ThreadRow = memo(function ThreadRow(props: ThreadRowProps) {
  const { item, href, folder, selected, focused, now, tz, mobile = false, density = 'default' } = props;
  const chip = rowStatusChip(item, folder, now, tz);
  const dateTs = folder === 'scheduled' && item.scheduled_at !== null ? item.scheduled_at : item.last_at;
  const date = formatListDate(dateTs, now, tz);
  const view = participantsView(item, folder);
  const ariaLabel = [
    item.unread ? t('mail.list.unreadRow') : '',
    (view.prefix ?? '') + view.parts.map((p) => p.text).join('，'),
    displaySubject(item.subject),
    item.snippet,
    item.has_attachments ? t('mail.list.hasAttachment') : '',
    date,
  ]
    .filter(Boolean)
    .join('，');

  const stop = (e: MouseEvent) => e.stopPropagation();
  const star = (
    <IconButton
      icon="star"
      fill={item.starred}
      label={item.starred ? t('mail.list.unstar') : t('mail.list.star')}
      size="sm"
      tooltip={false}
      className={cx('relative z-[1]', item.starred ? 'text-star hover:text-star' : 'text-on-surface-variant/70')}
      onClick={(e) => {
        stop(e);
        props.onStar(item);
      }}
    />
  );

  const rowClass = cx(
    'group relative flex border-b border-divider text-sm transition-shadow duration-75',
    item.unread ? 'row-unread' : 'row-read',
    selected && 'row-selected',
    'hover:z-[2] hover:shadow-row-hover',
    focused && 'shadow-[inset_3px_0_0_var(--color-primary)]',
  );

  const link = (
    <Link
      to={href}
      onClick={(e) => props.onOpen(item, e)}
      aria-label={ariaLabel}
      data-thread-link={item.id}
      className="absolute inset-0 z-0 focus-visible:outline-offset-[-2px]"
    />
  );

  if (mobile) {
    const first = item.participants.find((p) => !p.is_me) ?? item.participants[0];
    return (
      <div role="listitem" data-thread-id={item.id} aria-current={focused || undefined} className={cx(rowClass, 'gap-3 px-4 py-3')}>
        {link}
        <button
          type="button"
          role="checkbox"
          aria-checked={selected}
          aria-label={t('mail.list.select')}
          onClick={(e) => {
            stop(e);
            props.onSelect(item, !selected, e.shiftKey);
          }}
          className="relative z-[1] mt-0.5 shrink-0 rounded-full"
        >
          {selected ? (
            <span className="flex size-10 items-center justify-center rounded-full bg-primary text-on-primary">
              <Icon name="check" size={22} />
            </span>
          ) : (
            <Avatar name={first?.name} email={first?.email ?? '?'} size={40} decorative />
          )}
        </button>
        <div className="pointer-events-none relative min-w-0 flex-1">
          <div className="flex items-baseline gap-2">
            <Participants item={item} folder={folder} className="flex-1 text-[15px]" />
            <span className={cx('shrink-0 text-xs', item.unread ? 'font-bold text-on-surface' : 'font-normal text-on-surface-variant')}>
              {date}
            </span>
          </div>
          <ChipsAndSubject {...props} chip={chip} showSnippet={false} />
          <div className="flex items-center gap-2">
            <span className="min-w-0 flex-1 truncate font-normal text-on-surface-variant">{item.snippet}</span>
            {item.has_attachments && <Icon name="attach_file" size={16} className="text-on-surface-variant" />}
            <span className="pointer-events-auto -my-1 -mr-2">{star}</span>
          </div>
        </div>
      </div>
    );
  }

  const actions = rowActions(item, folder);
  return (
    <div
      role="listitem"
      data-thread-id={item.id}
      aria-current={focused || undefined}
      className={cx(rowClass, DENSITY_HEIGHT[density], 'items-stretch')}
    >
      {link}
      <div className="relative z-[1] flex shrink-0 items-center pl-2">
        <span className="flex size-8 items-center justify-center">
          <Checkbox
            checked={selected}
            aria-label={t('mail.list.select')}
            onClick={(e) => {
              e.preventDefault();
              props.onSelect(item, !selected, e.shiftKey);
            }}
          />
        </span>
        {star}
      </div>
      <div className={cx('pointer-events-none relative flex min-w-0 flex-1 items-center', item.attachments_preview.length ? 'py-1.5' : '')}>
        <Participants item={item} folder={folder} className="w-[168px] shrink-0 pl-2 pr-4 lg:w-[200px]" />
        <div className="flex min-w-0 flex-1 flex-col justify-center">
          <ChipsAndSubject {...props} chip={chip} showSnippet />
          {density !== 'compact' && <AttachmentChips item={item} />}
        </div>
        <span
          className={cx(
            'flex w-[92px] shrink-0 items-center justify-end gap-1 pr-4 text-xs group-hover:invisible',
            item.attachments_preview.length ? 'self-start pt-[3px]' : 'self-center',
            item.unread ? 'font-bold text-on-surface' : 'font-normal text-on-surface-variant',
          )}
          title={formatFullDate(dateTs, tz)}
        >
          {item.has_attachments && !item.attachments_preview.length && <Icon name="attach_file" size={16} className="text-on-surface-variant" />}
          {date}
        </span>
      </div>
      <div
        className="absolute right-2 top-0 z-[3] hidden h-10 items-center bg-inherit pl-2 group-hover:flex"
      >
        {actions.map((a) => (
          <IconButton
            key={a.action}
            icon={a.icon}
            label={a.label}
            size="sm"
            tooltipSide="top"
            onClick={(e) => {
              stop(e);
              props.onAction(item, a.action);
            }}
          />
        ))}
      </div>
    </div>
  );
});
