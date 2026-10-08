/**
 * Compose dock [WP-F]: every open compose window, docked bottom-right and laid out
 * right-to-left in `windows` order (DESIGN.md §5 "Compose store"). At most 3 are expanded
 * (store rule); the rest are minimized chips showing only their title bar. A maximized window
 * becomes a centered large modal over a backdrop (click it to leave full screen). On phones
 * the expanded window fills the screen (compose.css).
 *
 * The window itself (TipTap / ProseMirror, most of the editor weight) is a separate chunk
 * loaded on demand, so the inbox does not download or parse it up front. It is prefetched once
 * the browser is idle after start-up, so "写邮件" / `c` still opens instantly; until it arrives
 * a window shows its title bar and a spinner (or a retry button if the download failed).
 *
 * Contract: `export function ComposeDock()`, no props; it reads `useComposeStore` itself and
 * AppShell renders it exactly once inside the authenticated shell.
 */
import '@/styles/compose.css';
import { useEffect, useState } from 'react';
import { Button } from '@/components/common';
import { t } from '@/i18n/zh';
import { useComposeStore } from '@/stores/compose';
import { LoadingWindow, WindowShell } from './ComposeShell';
import type { ComposeTestOptions } from './ComposeWindow';

type ComposeWindowModule = typeof import('./ComposeWindow');

let loaded: ComposeWindowModule | null = null;
let loading: Promise<ComposeWindowModule> | null = null;

/** Loads the compose window chunk (once; a failed download is retried on the next call). */
export function preloadComposeWindow(): Promise<ComposeWindowModule> {
  if (loaded) return Promise.resolve(loaded);
  loading ??= import('./ComposeWindow').then(
    (m) => {
      loaded = m;
      return m;
    },
    (e: unknown) => {
      loading = null;
      throw e;
    },
  );
  return loading;
}

type IdleWindow = Window & {
  requestIdleCallback?: (cb: () => void, opts?: { timeout: number }) => number;
  cancelIdleCallback?: (id: number) => void;
};

/**
 * The compose window module: loaded right away while windows are open, otherwise prefetched
 * when the page is idle.
 */
function useComposeWindowModule(needed: boolean): { module: ComposeWindowModule | null; failed: boolean; retry: () => void } {
  const [module, setModule] = useState<ComposeWindowModule | null>(loaded);
  const [failed, setFailed] = useState(false);
  const [attempt, setAttempt] = useState(0);

  useEffect(() => {
    if (module) return;
    let alive = true;
    const load = () =>
      void preloadComposeWindow().then(
        (m) => {
          if (alive) setModule(m);
        },
        () => {
          if (alive && needed) setFailed(true);
        },
      );
    if (needed) {
      load();
      return () => {
        alive = false;
      };
    }
    const w = window as IdleWindow;
    let cancel: () => void;
    if (typeof w.requestIdleCallback === 'function') {
      const id = w.requestIdleCallback(load, { timeout: 5000 });
      cancel = () => w.cancelIdleCallback?.(id);
    } else {
      const id = setTimeout(load, 2000);
      cancel = () => clearTimeout(id);
    }
    return () => {
      alive = false;
      cancel();
    };
  }, [module, needed, attempt]);

  return {
    module,
    failed: failed && !module,
    retry: () => {
      setFailed(false);
      setAttempt((n) => n + 1);
    },
  };
}

export function ComposeDock({ testOptions }: { testOptions?: ComposeTestOptions } = {}) {
  const windows = useComposeStore((s) => s.windows);
  const toggleMaximize = useComposeStore((s) => s.toggleMaximize);
  const { module, failed, retry } = useComposeWindowModule(windows.length > 0);
  if (windows.length === 0) return null;
  const maximized = windows.find((w) => w.maximized && !w.minimized);
  const ComposeWindow = module?.ComposeWindow;
  return (
    <div className="compose-dock" role="region" aria-label={t('compose.dockLabel')}>
      {maximized && (
        <div
          className="compose-backdrop"
          aria-hidden="true"
          data-testid="compose-backdrop"
          onClick={() => toggleMaximize(maximized.key)}
        />
      )}
      {windows.map((w) =>
        ComposeWindow ? (
          <ComposeWindow key={w.key} win={w} testOptions={testOptions} />
        ) : failed ? (
          <WindowShell key={w.key} win={w}>
            <span>{t('compose.editorLoadFailed')}</span>
            <Button variant="tonal" size="sm" onClick={retry}>
              {t('actions.retry')}
            </Button>
          </WindowShell>
        ) : (
          <LoadingWindow key={w.key} win={w} />
        ),
      )}
    </div>
  );
}
