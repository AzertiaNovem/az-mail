/**
 * Authenticated layout [WP-E]: top bar, collapsible sidebar (icon rail under 1024 px or when
 * collapsed, expanding on hover; a drawer on phones), the rounded main card on the #f6f8fc
 * background, and the app-wide pieces mounted exactly once: ComposeDock [F], the label dialog,
 * the shortcuts help dialog, the realtime socket and the global keyboard shortcuts.
 * (ToastHost is mounted once by main.tsx, outside the router.)
 */
import '@/styles/mail.css';
import { useQueryClient } from '@tanstack/react-query';
import { useEffect, useRef, useState } from 'react';
import { Outlet, useLocation, useNavigate } from 'react-router';
import { cx } from '@/components/common';
import { ComposeDock } from '@/components/compose/ComposeDock';
import { openNewMessage } from '@/components/mail/compose';
import { LabelDialogHost } from '@/components/mail/LabelDialog';
import { t } from '@/i18n/zh';
import { installShortcutListener, useShortcuts } from '@/lib/keyboard';
import { useUiStore } from '@/stores/ui';
import { startRealtime } from '@/ws/socket';
import { ShortcutsHelp } from './ShortcutsHelp';
import { Sidebar } from './Sidebar';
import { TopBar } from './TopBar';
import { useIsMobile, useIsNarrow } from './useMediaQuery';

const HOVER_EXPAND_DELAY_MS = 150;

export function AppShell() {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const location = useLocation();
  const mobile = useIsMobile();
  const narrow = useIsNarrow();
  const collapsedPref = useUiStore((s) => s.sidebarCollapsed);
  const drawerOpen = useUiStore((s) => s.mobileNavOpen);
  const [hovered, setHovered] = useState(false);
  const hoverTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => startRealtime(qc), [qc]);
  useEffect(() => installShortcutListener(), []);

  // Close the drawer on navigation and when leaving the phone layout.
  useEffect(() => {
    useUiStore.getState().setMobileNavOpen(false);
  }, [location.pathname, mobile]);

  // The open drawer takes focus and closes on Escape.
  const drawerRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!mobile || !drawerOpen) return;
    drawerRef.current?.querySelector<HTMLElement>('button, a[href]')?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') useUiStore.getState().setMobileNavOpen(false);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [mobile, drawerOpen]);

  useShortcuts({
    compose: () => openNewMessage(),
    search: () => useUiStore.getState().requestSearchFocus(),
    help: () => useUiStore.getState().setShortcutsHelpOpen(true),
    goInbox: () => void navigate('/mail/inbox'),
    goStarred: () => void navigate('/mail/starred'),
    goSent: () => void navigate('/mail/sent'),
    goDrafts: () => void navigate('/mail/drafts'),
    goAll: () => void navigate('/mail/all'),
  });

  const collapsed = narrow || collapsedPref;
  const expandedOverlay = collapsed && hovered;
  const onMenu = () => {
    const st = useUiStore.getState();
    if (mobile) st.setMobileNavOpen(!st.mobileNavOpen);
    else if (narrow) setHovered((h) => !h);
    else st.toggleSidebar();
  };

  const enter = () => {
    if (!collapsed) return;
    if (hoverTimer.current) clearTimeout(hoverTimer.current);
    hoverTimer.current = setTimeout(() => setHovered(true), HOVER_EXPAND_DELAY_MS);
  };
  const leave = () => {
    if (hoverTimer.current) clearTimeout(hoverTimer.current);
    hoverTimer.current = null;
    setHovered(false);
  };
  useEffect(() => () => {
    if (hoverTimer.current) clearTimeout(hoverTimer.current);
  }, []);

  return (
    <div className="azm-shell flex h-dvh flex-col overflow-hidden bg-surface text-on-surface">
      <TopBar onMenu={onMenu} />
      <div className="flex min-h-0 flex-1">
        {!mobile && (
          // Hover only widens the rail visually; the <nav> inside is the landmark.
          <div
            className={cx('relative shrink-0 transition-[width] duration-200 ease-[var(--ease-emphasized)]', collapsed ? 'w-[72px]' : 'w-[256px]')}
            onMouseEnter={enter}
            onMouseLeave={leave}
          >
            <div
              className={cx(
                'absolute inset-y-0 left-0 z-30 overflow-y-auto overflow-x-hidden bg-surface',
                expandedOverlay ? 'w-[256px] rounded-r-2xl shadow-elevation-2' : 'w-full',
              )}
            >
              <Sidebar compact={collapsed && !expandedOverlay} onNavigate={leave} />
            </div>
          </div>
        )}
        <main className="flex min-w-0 flex-1 flex-col md:pb-4 md:pr-4">
          <div className="relative flex min-h-0 flex-1 flex-col overflow-hidden bg-surface-container md:rounded-card">
            <div className="min-h-0 flex-1 overflow-y-auto">
              <Outlet />
            </div>
          </div>
        </main>
      </div>

      {mobile && (
        <div className={cx('fixed inset-0 z-[700]', drawerOpen ? 'pointer-events-auto' : 'pointer-events-none')} aria-hidden={!drawerOpen}>
          <button
            type="button"
            tabIndex={-1}
            aria-label={t('actions.close')}
            className={cx('absolute inset-0 cursor-default bg-scrim transition-opacity duration-200', drawerOpen ? 'opacity-100' : 'opacity-0')}
            onClick={() => useUiStore.getState().setMobileNavOpen(false)}
          />
          <div
            ref={drawerRef}
            role="dialog"
            aria-modal="true"
            aria-label={t('mail.shell.mainMenu')}
            className={cx(
              'absolute inset-y-0 left-0 w-[280px] max-w-[85vw] overflow-y-auto rounded-r-2xl bg-surface pt-3 shadow-elevation-3 transition-transform duration-200',
              drawerOpen ? 'translate-x-0' : '-translate-x-full',
            )}
            inert={!drawerOpen}
          >
            <Sidebar compact={false} onNavigate={() => useUiStore.getState().setMobileNavOpen(false)} />
          </div>
        </div>
      )}

      <ComposeDock />
      <LabelDialogHost />
      <ShortcutsHelp />
    </div>
  );
}
