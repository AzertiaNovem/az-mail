/**
 * Left navigation [WP-E]: 写邮件 FAB, folders with unread / draft counts from `['counts']`,
 * 更多 (所有邮件 / 垃圾邮件 / 已删除), and the 标签 section (colored dots, "+", context menus).
 * `compact` renders the icon rail (sidebar collapsed or narrow screens).
 */
import { useState } from 'react';
import { NavLink, useLocation } from 'react-router';
import { threadListFilter } from '@/api/queryKeys';
import type { FolderId } from '@/api/types';
import { cx, Icon, IconButton, Tooltip } from '@/components/common';
import { openNewMessage } from '@/components/mail/compose';
import { openLabelDialog } from '@/components/mail/LabelDialog';
import { useCounts, useLabels } from '@/components/mail/queries';
import { folderName, t } from '@/i18n/zh';
import { formatCount } from '@/lib/format';
import { listScope, useUiStore } from '@/stores/ui';
import { LabelNavItem } from './LabelNavItem';
import { navItemClass } from './navStyles';

interface FolderEntry {
  folder: FolderId;
  icon: string;
}

export const PRIMARY_FOLDERS: FolderEntry[] = [
  { folder: 'inbox', icon: 'inbox' },
  { folder: 'starred', icon: 'star' },
  { folder: 'sent', icon: 'send' },
  { folder: 'drafts', icon: 'draft' },
  { folder: 'scheduled', icon: 'schedule_send' },
];

export const MORE_FOLDERS: FolderEntry[] = [
  { folder: 'all', icon: 'stacked_email' },
  { folder: 'spam', icon: 'report' },
  { folder: 'trash', icon: 'delete' },
];

/** Count shown next to a folder: unread for inbox / spam, totals for drafts / scheduled. */
export function folderBadge(folder: FolderId, counts: { inbox_unread: number; drafts: number; scheduled: number; spam_unread: number } | undefined) {
  if (!counts) return { value: 0, bold: false };
  switch (folder) {
    case 'inbox':
      return { value: counts.inbox_unread, bold: true };
    case 'drafts':
      return { value: counts.drafts, bold: false };
    case 'scheduled':
      return { value: counts.scheduled, bold: false };
    case 'spam':
      return { value: counts.spam_unread, bold: true };
    default:
      return { value: 0, bold: false };
  }
}

export interface SidebarProps {
  compact: boolean;
  /** Called after a navigation (closes the mobile drawer). */
  onNavigate?: () => void;
}

export function Sidebar({ compact, onNavigate }: SidebarProps) {
  const location = useLocation();
  const counts = useCounts().data;
  const labels = useLabels().data ?? [];
  const inMore = MORE_FOLDERS.some((f) => location.pathname === `/mail/${f.folder}` || location.pathname.startsWith(`/mail/${f.folder}/`));
  const [moreToggled, setShowMore] = useState(false);
  // Always expanded while one of its folders is open.
  const showMore = moreToggled || inMore;

  const navigateTo = (folder: FolderId) => {
    // Clicking a folder returns to its first page, like Gmail.
    const st = useUiStore.getState();
    st.resetPages(listScope(threadListFilter({ folder })));
    st.clearSelection();
    onNavigate?.();
  };

  const folderItem = ({ folder, icon }: FolderEntry) => {
    const badge = folderBadge(folder, counts);
    const name = folderName(folder);
    const link = (
      <NavLink
        key={folder}
        to={`/mail/${folder}`}
        onClick={() => navigateTo(folder)}
        aria-label={compact ? name : undefined}
        className={({ isActive }) => cx(navItemClass(isActive, compact), 'relative')}
      >
        {({ isActive }) => (
          <>
            <Icon name={icon} size={20} fill={isActive} className={isActive ? 'text-on-nav-selected' : 'text-on-surface-variant'} />
            {!compact && <span className="min-w-0 flex-1 truncate">{name}</span>}
            {/* The rail only flags unread counts (inbox, spam), like Gmail. */}
            {badge.value > 0 && (!compact || badge.bold) &&
              (compact ? (
                <span className="absolute right-1.5 top-0.5 min-w-4 rounded-full bg-error px-1 text-center text-[10px] font-bold leading-4 text-white">
                  {badge.value > 99 ? '99+' : badge.value}
                </span>
              ) : (
                <span
                  className={cx('ml-2 shrink-0 text-xs', badge.bold ? 'font-bold' : 'font-normal')}
                  aria-label={
                    folder === 'drafts' ? t('mail.nav.draftCount', { count: badge.value }) : t('mail.nav.unreadCount', { count: badge.value })
                  }
                >
                  {formatCount(badge.value)}
                </span>
              ))}
          </>
        )}
      </NavLink>
    );
    return compact ? (
      <Tooltip key={folder} label={badge.value > 0 ? `${name} (${formatCount(badge.value)})` : name} side="right">
        {/* A wrapper: Radix Slot cannot merge NavLink's function className. */}
        <span className="block">{link}</span>
      </Tooltip>
    ) : (
      link
    );
  };

  return (
    <nav className="flex h-full flex-col pb-4" aria-label={t('mail.shell.mainMenu')}>
      <div className={cx('pb-4 pt-2', compact ? 'flex justify-center' : 'pl-2')}>
        <Tooltip label={compact ? t('mail.nav.compose') : null} side="right">
          <button
            type="button"
            onClick={() => {
              openNewMessage();
              onNavigate?.();
            }}
            aria-label={t('mail.nav.compose')}
            className={cx(
              'flex h-14 items-center rounded-2xl bg-compose text-sm font-medium text-on-compose transition-[background-color,box-shadow] duration-150',
              'hover:bg-compose-hover hover:shadow-elevation-1 active:shadow-none',
              compact ? 'w-14 justify-center' : 'min-w-[138px] gap-3 pl-4 pr-6',
            )}
          >
            <Icon name="edit" size={24} />
            {!compact && <span>{t('mail.nav.compose')}</span>}
          </button>
        </Tooltip>
      </div>

      <div className="flex flex-col gap-px">
        {PRIMARY_FOLDERS.map(folderItem)}
        {!compact ? (
          <button
            type="button"
            onClick={() => setShowMore(!showMore)}
            aria-expanded={showMore}
            className={cx(navItemClass(false, false), 'text-left')}
          >
            <Icon name={showMore ? 'keyboard_arrow_up' : 'keyboard_arrow_down'} size={20} className="text-on-surface-variant" />
            <span>{showMore ? t('mail.nav.less') : t('mail.nav.more')}</span>
          </button>
        ) : (
          <Tooltip label={showMore ? t('mail.nav.less') : t('mail.nav.more')} side="right">
            <button
              type="button"
              onClick={() => setShowMore(!showMore)}
              aria-expanded={showMore}
              aria-label={showMore ? t('mail.nav.less') : t('mail.nav.more')}
              className={navItemClass(false, true)}
            >
              <Icon name={showMore ? 'keyboard_arrow_up' : 'keyboard_arrow_down'} size={20} className="text-on-surface-variant" />
            </button>
          </Tooltip>
        )}
        {showMore && MORE_FOLDERS.map(folderItem)}
      </div>

      <div className={cx('mt-4 flex items-center', compact ? 'justify-center' : 'pl-[26px] pr-5')}>
        {!compact && <h2 className="flex-1 text-base font-medium text-on-surface">{t('mail.nav.labels')}</h2>}
        <IconButton icon="add" label={t('mail.nav.createLabel')} size="sm" onClick={() => openLabelDialog({ mode: 'create' })} />
      </div>
      <div className="mt-1 flex flex-col gap-px">
        {labels.map((l) => (
          <LabelNavItem
            key={l.id}
            label={l}
            unread={counts?.labels[String(l.id)]?.unread ?? 0}
            compact={compact}
            onNavigate={() => {
              const st = useUiStore.getState();
              st.resetPages(listScope(threadListFilter({ labelId: l.id })));
              st.clearSelection();
              onNavigate?.();
            }}
          />
        ))}
      </div>
    </nav>
  );
}
