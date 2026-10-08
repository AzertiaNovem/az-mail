/**
 * Compose window chrome [WP-F]: the title bar (title · minimize · full screen · close) and the
 * loading / error shell. Kept apart from ComposeWindow so the dock can show a working window
 * while the editor chunk (TipTap / ProseMirror, loaded on demand) is still downloading.
 */
import { useId, type ReactNode } from 'react';
import { cx, IconButton, Spinner } from '@/components/common';
import { t } from '@/i18n/zh';
import { useComposeStore, type ComposeWin } from '@/stores/compose';

interface TitleBarProps {
  win: ComposeWin;
  titleId: string;
  title: string;
  onClose: () => void;
  busy?: boolean;
}

export function TitleBar({ win, titleId, title, onClose, busy }: TitleBarProps) {
  const toggleMinimize = useComposeStore((s) => s.toggleMinimize);
  const toggleMaximize = useComposeStore((s) => s.toggleMaximize);
  return (
    <header className="cw-titlebar">
      <h2 id={titleId} className="cw-title">
        <button
          type="button"
          className="cw-title-button"
          aria-expanded={!win.minimized}
          onClick={() => toggleMinimize(win.key)}
        >
          {busy ? t('compose.closing') : title}
        </button>
      </h2>
      <div className="cw-title-actions">
        <IconButton
          icon={win.minimized ? 'keyboard_arrow_up' : 'remove'}
          label={win.minimized ? t('compose.restore') : t('compose.minimize')}
          size="sm"
          tooltipSide="top"
          onClick={() => toggleMinimize(win.key)}
        />
        <IconButton
          icon={win.maximized ? 'close_fullscreen' : 'open_in_full'}
          label={win.maximized ? t('compose.exitMaximize') : t('compose.maximize')}
          size="sm"
          iconSize={16}
          tooltipSide="top"
          onClick={() => toggleMaximize(win.key)}
        />
        <IconButton icon="close" label={t('compose.close')} size="sm" tooltipSide="top" onClick={onClose} disabled={busy} />
      </div>
    </header>
  );
}

export function windowClass(win: ComposeWin, focused: boolean) {
  return cx('cw', win.minimized && 'is-min', win.maximized && !win.minimized && 'is-max', focused && 'is-focused');
}

/** Loading / error state with a working title bar. */
export function WindowShell({ win, children }: { win: ComposeWin; children: ReactNode }) {
  const titleId = useId();
  const focused = useComposeStore((s) => s.focusedKey === win.key);
  const close = useComposeStore((s) => s.close);
  return (
    <section className={windowClass(win, focused)} role="dialog" aria-labelledby={titleId} data-testid="compose-window">
      <TitleBar win={win} titleId={titleId} title={win.title || t('compose.newMessage')} onClose={() => close(win.key)} />
      <div className="cw-body" hidden={win.minimized}>
        <div className="flex flex-1 flex-col items-center justify-center gap-3 p-6 text-center text-sm text-on-surface-variant">
          {children}
        </div>
      </div>
    </section>
  );
}

/** "正在加载…" window (seed loading, or the editor chunk still downloading). */
export function LoadingWindow({ win }: { win: ComposeWin }) {
  return (
    <WindowShell win={win}>
      <Spinner />
      <span>{t('compose.loading')}</span>
    </WindowShell>
  );
}
