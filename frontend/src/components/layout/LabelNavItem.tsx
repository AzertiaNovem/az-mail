/**
 * Sidebar label entry [WP-E]: colored dot, name, unread count, and a context menu (⋮ on hover
 * or right-click) with 标签颜色 / 修改 / 删除标签.
 */
import { useQueryClient } from '@tanstack/react-query';
import { useState } from 'react';
import { NavLink, useLocation, useNavigate } from 'react-router';
import { errorMessage } from '@/api/client';
import { deleteLabel, updateLabel } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Label } from '@/api/types';
import { ConfirmDialog, cx, DropdownMenu, IconButton, Tooltip, type MenuEntry } from '@/components/common';
import { openLabelDialog, upsertCachedLabel } from '@/components/mail/LabelDialog';
import { t } from '@/i18n/zh';
import { formatCount } from '@/lib/format';
import { LABEL_COLORS } from '@/lib/color';
import { toast } from '@/stores/toast';
import { navItemClass } from './navStyles';

export interface LabelNavItemProps {
  label: Label;
  unread: number;
  compact: boolean;
  onNavigate?: () => void;
}

export function LabelNavItem({ label, unread, compact, onNavigate }: LabelNavItemProps) {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const location = useLocation();
  const [menuOpen, setMenuOpen] = useState(false);
  const [confirm, setConfirm] = useState(false);
  const [busy, setBusy] = useState(false);
  const path = `/mail/label/${label.id}`;

  const setColor = async (color: string) => {
    try {
      const saved = await updateLabel(label.id, { color });
      qc.setQueryData<Label[]>(queryKeys.labels(), (old) => upsertCachedLabel(old, saved));
      void qc.invalidateQueries({ queryKey: queryKeys.labels() });
    } catch (e) {
      toast.error(errorMessage(e));
    }
  };

  const remove = async () => {
    setBusy(true);
    try {
      await deleteLabel(label.id);
      qc.setQueryData<Label[]>(queryKeys.labels(), (old) => (old ?? []).filter((l) => l.id !== label.id));
      void qc.invalidateQueries({ queryKey: queryKeys.labels() });
      void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
      void qc.invalidateQueries({ queryKey: queryKeys.threadAll() });
      void qc.invalidateQueries({ queryKey: queryKeys.counts() });
      toast.push({ message: t('mail.labels.deleted', { name: label.name }) });
      setConfirm(false);
      if (location.pathname === path || location.pathname.startsWith(`${path}/`)) void navigate('/mail/inbox', { replace: true });
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  const items: MenuEntry[] = [
    {
      type: 'submenu',
      key: 'color',
      label: t('mail.labels.menuColor'),
      icon: 'palette',
      items: LABEL_COLORS.map((c) => ({
        key: c,
        label: (
          <span className="flex items-center gap-2">
            <span className="size-4 rounded-full" style={{ backgroundColor: c }} aria-hidden="true" />
            <span className={cx(c.toLowerCase() === label.color.toLowerCase() && 'font-bold')}>{t('mail.labels.colorOption', { color: c })}</span>
          </span>
        ),
        onSelect: () => void setColor(c),
      })),
    },
    { key: 'edit', label: t('mail.labels.menuEdit'), icon: 'edit', onSelect: () => openLabelDialog({ mode: 'edit', label }) },
    { key: 'delete', label: t('mail.labels.menuDelete'), icon: 'delete', danger: true, onSelect: () => setConfirm(true) },
  ];

  const link = (
    <NavLink
      to={path}
      onClick={onNavigate}
      onContextMenu={(e) => {
        e.preventDefault();
        setMenuOpen(true);
      }}
      aria-label={compact ? label.name : undefined}
      className={({ isActive }) => navItemClass(isActive, compact)}
    >
      <span className="flex size-5 shrink-0 items-center justify-center" aria-hidden="true">
        <span className="size-2.5 rounded-full" style={{ backgroundColor: label.color }} />
      </span>
      {!compact && (
        <>
          <span className="min-w-0 flex-1 truncate">{label.name}</span>
          {unread > 0 && (
            <span className="ml-2 shrink-0 text-xs font-bold group-hover/label:invisible" aria-label={t('mail.nav.unreadCount', { count: unread })}>
              {formatCount(unread)}
            </span>
          )}
        </>
      )}
    </NavLink>
  );

  return (
    <div className="group/label relative">
      {compact ? (
        <Tooltip label={label.name} side="right">
          <span className="block">{link}</span>
        </Tooltip>
      ) : (
        link
      )}
      {!compact && (
        <DropdownMenu
          open={menuOpen}
          onOpenChange={setMenuOpen}
          align="start"
          items={items}
          aria-label={t('mail.nav.labelOptions', { name: label.name })}
          trigger={
            <IconButton
              icon="more_vert"
              label={t('mail.nav.labelOptions', { name: label.name })}
              size="sm"
              tooltip={false}
              className="absolute right-5 top-0 size-8 opacity-0 focus-visible:opacity-100 group-hover/label:opacity-100 data-[state=open]:opacity-100"
            />
          }
        />
      )}
      <ConfirmDialog
        open={confirm}
        onOpenChange={setConfirm}
        title={t('mail.labels.deleteTitle', { name: label.name })}
        message={t('mail.labels.deleteMessage')}
        confirmLabel={t('actions.delete')}
        danger
        busy={busy}
        onConfirm={() => void remove()}
      />
    </div>
  );
}
