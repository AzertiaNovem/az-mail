import { Dialog } from '@/components/common';
import { t } from '@/i18n/zh';
import { SHORTCUT_HELP, type ShortcutJoin } from '@/lib/keyboard';
import { useUiStore } from '@/stores/ui';

const GROUPS = ['navigation', 'actions', 'thread'] as const;

function Keys({ keys, join = 'plus' }: { keys: string[]; join?: ShortcutJoin }) {
  const sep = join === 'plus' ? '+' : join === 'or' ? t('mail.shortcuts.or') : t('mail.shortcuts.seqThen');
  return (
    <span className="flex shrink-0 items-center gap-1">
      {keys.map((k, i) => (
        <span key={i} className="flex items-center gap-1">
          {i > 0 && <span className="text-xs text-on-surface-variant">{sep}</span>}
          <kbd className="inline-flex h-6 min-w-6 items-center justify-center rounded border border-outline-variant bg-surface-container-low px-1.5 font-mono text-xs text-on-surface">
            {k}
          </kbd>
        </span>
      ))}
    </span>
  );
}

/** "?" dialog listing the keyboard shortcuts. */
export function ShortcutsHelp() {
  const open = useUiStore((s) => s.shortcutsHelpOpen);
  const setOpen = useUiStore((s) => s.setShortcutsHelpOpen);
  return (
    <Dialog open={open} onOpenChange={setOpen} title={t('mail.shortcuts.title')} description={t('mail.shortcuts.hint')} size="lg">
      <div className="grid gap-6 pb-2 sm:grid-cols-3">
        {GROUPS.map((g) => (
          <section key={g}>
            <h3 className="mb-2 text-sm font-medium text-on-surface">{t(`mail.shortcuts.groups.${g}`)}</h3>
            <ul className="flex flex-col gap-2">
              {SHORTCUT_HELP.filter((s) => s.group === g).map((s) => (
                <li key={s.label} className="flex items-center justify-between gap-3 text-sm text-on-surface-variant">
                  <span>{t(s.label)}</span>
                  <Keys keys={s.keys} join={s.join} />
                </li>
              ))}
            </ul>
          </section>
        ))}
      </div>
    </Dialog>
  );
}
