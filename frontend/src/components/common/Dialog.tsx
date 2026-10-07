import * as D from '@radix-ui/react-dialog';
import type { ReactNode } from 'react';
import { t } from '@/i18n/zh';
import { Button } from './Button';
import { cx } from './cx';
import { IconButton } from './IconButton';

export * as DialogPrimitive from '@radix-ui/react-dialog';

export interface DialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  /** Required for accessibility; use `hideTitle` to keep it screen-reader only. */
  title: ReactNode;
  hideTitle?: boolean;
  description?: ReactNode;
  children?: ReactNode;
  /** Action row, right-aligned. */
  footer?: ReactNode;
  size?: 'sm' | 'md' | 'lg' | 'xl';
  /** Show the × button in the header (default true). */
  showClose?: boolean;
  className?: string;
  /** Prevent closing on outside click / Escape (e.g. while saving). */
  dismissible?: boolean;
}

const widths = { sm: 'max-w-sm', md: 'max-w-md', lg: 'max-w-2xl', xl: 'max-w-4xl' } as const;

/** Modal dialog (Radix): scrim, focus trap, Escape to close. */
export function Dialog({
  open,
  onOpenChange,
  title,
  hideTitle = false,
  description,
  children,
  footer,
  size = 'md',
  showClose = true,
  className,
  dismissible = true,
}: DialogProps) {
  const block = (e: Event) => {
    if (!dismissible) e.preventDefault();
  };
  return (
    <D.Root open={open} onOpenChange={onOpenChange}>
      <D.Portal>
        <D.Overlay className="fixed inset-0 z-[900] bg-scrim animate-fade-in" />
        <D.Content
          // Without a Description, opt out explicitly (silences Radix's a11y warning).
          {...(description ? {} : { 'aria-describedby': undefined })}
          onEscapeKeyDown={block}
          onPointerDownOutside={block}
          onInteractOutside={block}
          className={cx(
            'fixed left-1/2 top-1/2 z-[901] flex max-h-[calc(100vh-64px)] w-[calc(100vw-32px)] -translate-x-1/2 -translate-y-1/2 flex-col',
            'rounded-3xl bg-surface-container text-on-surface shadow-elevation-3 outline-none animate-scale-in',
            widths[size],
            className,
          )}
        >
          <div className={cx('flex items-start gap-2 px-6 pt-6', hideTitle && 'sr-only')}>
            <D.Title className="min-w-0 flex-1 text-xl font-normal leading-8 text-on-surface">{title}</D.Title>
            {showClose && !hideTitle && (
              <D.Close asChild>
                <IconButton icon="close" label={t('actions.close')} size="sm" className="-mr-2 -mt-1" tooltip={false} />
              </D.Close>
            )}
          </div>
          {description && (
            // A <div>, not Radix's default <p>: descriptions may contain block content (lists, paragraphs).
            <D.Description asChild>
              <div className="px-6 pt-2 text-sm text-on-surface-variant">{description}</div>
            </D.Description>
          )}
          {children !== undefined && <div className="min-h-0 flex-1 overflow-y-auto px-6 pt-4">{children}</div>}
          {footer ? (
            <div className="flex shrink-0 items-center justify-end gap-2 px-6 pb-5 pt-5">{footer}</div>
          ) : (
            <div className="pb-6" />
          )}
        </D.Content>
      </D.Portal>
    </D.Root>
  );
}

export interface ConfirmDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: ReactNode;
  message?: ReactNode;
  confirmLabel?: string;
  cancelLabel?: string;
  /** Destructive confirmation (e.g. 永久删除). */
  danger?: boolean;
  /** Shows a spinner on the confirm button and blocks dismissal. */
  busy?: boolean;
  onConfirm: () => void;
}

/** Two-button confirmation built on Dialog. */
export function ConfirmDialog({
  open,
  onOpenChange,
  title,
  message,
  confirmLabel = t('actions.confirm'),
  cancelLabel = t('actions.cancel'),
  danger = false,
  busy = false,
  onConfirm,
}: ConfirmDialogProps) {
  return (
    <Dialog
      open={open}
      onOpenChange={onOpenChange}
      title={title}
      description={message}
      size="sm"
      showClose={false}
      dismissible={!busy}
      footer={
        <>
          <Button variant="text" onClick={() => onOpenChange(false)} disabled={busy}>
            {cancelLabel}
          </Button>
          <Button
            variant={danger ? 'text' : 'primary'}
            danger={danger}
            loading={busy}
            onClick={onConfirm}
            autoFocus
          >
            {confirmLabel}
          </Button>
        </>
      }
    />
  );
}
