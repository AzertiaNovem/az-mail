/**
 * Compose dock [WP-F]: every open compose window, docked bottom-right and laid out
 * right-to-left in `windows` order (DESIGN.md §5 "Compose store"). At most 3 are expanded
 * (store rule); the rest are minimized chips showing only their title bar. A maximized window
 * becomes a centered large modal over a backdrop (click it to leave full screen). On phones
 * the expanded window fills the screen (compose.css).
 *
 * Contract: `export function ComposeDock()`, no props; it reads `useComposeStore` itself and
 * AppShell renders it exactly once inside the authenticated shell.
 */
import '@/styles/compose.css';
import { t } from '@/i18n/zh';
import { useComposeStore } from '@/stores/compose';
import { ComposeWindow, type ComposeTestOptions } from './ComposeWindow';

export function ComposeDock({ testOptions }: { testOptions?: ComposeTestOptions } = {}) {
  const windows = useComposeStore((s) => s.windows);
  const toggleMaximize = useComposeStore((s) => s.toggleMaximize);
  if (windows.length === 0) return null;
  const maximized = windows.find((w) => w.maximized && !w.minimized);
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
      {windows.map((w) => (
        <ComposeWindow key={w.key} win={w} testOptions={testOptions} />
      ))}
    </div>
  );
}
