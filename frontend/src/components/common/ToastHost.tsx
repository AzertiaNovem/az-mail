import { t } from '@/i18n/zh';
import { useToastStore } from '@/stores/toast';
import { cx } from './cx';
import { IconButton } from './IconButton';

/** Renders the global toast store bottom-left (Gmail snackbar). Mount once at the app root. */
export function ToastHost() {
  const toasts = useToastStore((s) => s.toasts);
  const dismiss = useToastStore((s) => s.dismiss);
  return (
    <div
      aria-live="polite"
      aria-label="通知"
      role="region"
      className="pointer-events-none fixed bottom-6 left-6 z-[1100] flex flex-col items-start gap-2"
    >
      {toasts.map((toast) => (
        <div
          key={toast.id}
          role={toast.tone === 'error' ? 'alert' : undefined}
          className={cx(
            'pointer-events-auto flex min-h-12 min-w-[288px] max-w-[min(568px,calc(100vw-48px))] items-center gap-1',
            'rounded bg-inverse-surface py-1 pl-4 pr-1 text-sm text-inverse-on-surface shadow-elevation-3 animate-toast-in',
          )}
        >
          {toast.tone === 'error' && <span className="sr-only">错误：</span>}
          <span className="min-w-0 flex-1 py-2 pr-2">{toast.message}</span>
          {toast.actions.map((a, i) => (
            <button
              key={`${i}:${a.label}`}
              type="button"
              className="h-9 shrink-0 rounded px-3 font-medium text-inverse-primary transition-colors hover:bg-white/10 focus-visible:bg-white/16"
              onClick={() => {
                a.onClick();
                if (!a.keepOpen) dismiss(toast.id);
              }}
            >
              {a.label}
            </button>
          ))}
          <IconButton
            icon="close"
            label={t('actions.close')}
            size="sm"
            tone="inverse"
            tooltip={false}
            onClick={() => dismiss(toast.id)}
          />
        </div>
      ))}
    </div>
  );
}
