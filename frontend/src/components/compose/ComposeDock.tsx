/**
 * Compose dock [WP-F]. PLACEHOLDER written by WP0 so AppShell (WP-E) can render it; WP-F
 * replaces it with the real windows (ComposeWindow) and minimized chips.
 *
 * Contract (kept by the replacement): `export function ComposeDock()`, no props. It reads
 * `useComposeStore` itself; AppShell renders it exactly once, inside the authenticated shell.
 * Windows dock bottom-right, right-to-left in `windows` order (DESIGN.md §5 "Compose store").
 */
import '@/styles/compose.css';
import { IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import { useComposeStore } from '@/stores/compose';

export function ComposeDock() {
  const windows = useComposeStore((s) => s.windows);
  const close = useComposeStore((s) => s.close);
  if (windows.length === 0) return null;
  return (
    <div className="compose-dock" role="region" aria-label={t('common.newMessage')}>
      {windows.map((w) => (
        <div key={w.key} className="compose-dock-chip">
          <span className="min-w-0 flex-1 truncate">{w.title || t('common.newMessage')}</span>
          <IconButton icon="close" label={t('actions.close')} size="sm" tooltip={false} onClick={() => close(w.key)} />
        </div>
      ))}
    </div>
  );
}
