import { useId, type ComponentProps, type ReactNode } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';

// ───────────── shared field chrome (TextField, Select) ─────────────

export interface FieldChromeProps {
  /** Visible label above the control. Without it, pass `aria-label`. */
  label?: ReactNode;
  /** Error message below the control; also sets `aria-invalid`. Takes precedence over `helperText`. */
  error?: ReactNode;
  /** Hint below the control. */
  helperText?: ReactNode;
  /** Wrapper (label + control + message) class. */
  containerClassName?: string;
}

/** Ids and ARIA attributes linking a control to its label and message. */
export function useFieldA11y(
  id: string | undefined,
  error: ReactNode,
  helperText: ReactNode,
  describedBy: string | undefined,
) {
  const autoId = useId();
  const controlId = id ?? autoId;
  const hasError = error !== undefined && error !== null && error !== false && error !== '';
  const hasHelper = !hasError && helperText !== undefined && helperText !== null && helperText !== '';
  const messageId = hasError || hasHelper ? `${controlId}-msg` : undefined;
  const ariaDescribedBy = [describedBy, messageId].filter(Boolean).join(' ') || undefined;
  return { controlId, hasError, messageId, ariaDescribedBy };
}

export function FieldLabel({ htmlFor, children }: { htmlFor: string; children: ReactNode }) {
  return (
    <label htmlFor={htmlFor} className="text-sm font-medium text-on-surface-variant">
      {children}
    </label>
  );
}

export function FieldMessage({ id, error, children }: { id: string; error: boolean; children: ReactNode }) {
  return (
    <p id={id} className={cx('text-xs leading-4', error ? 'text-error' : 'text-on-surface-variant')}>
      {children}
    </p>
  );
}

/** Border / focus classes of an outlined field box. */
export const fieldBoxClass = (error: boolean, disabled: boolean | undefined) =>
  cx(
    'flex h-10 items-center gap-2 rounded border bg-surface-container px-3 text-sm text-on-surface transition-colors',
    error
      ? 'border-error focus-within:ring-1 focus-within:ring-error'
      : 'border-outline-variant hover:border-outline focus-within:border-primary focus-within:ring-1 focus-within:ring-primary',
    disabled && 'pointer-events-none opacity-40',
  );

// ───────────── TextField ─────────────

export interface TextFieldProps extends Omit<ComponentProps<'input'>, 'size'>, FieldChromeProps {
  /** Leading Material Symbol (e.g. "search"). */
  leadingIcon?: string;
  /** Trailing content inside the box (e.g. an IconButton). */
  trailing?: ReactNode;
}

/**
 * Outlined text input with label, helper / error text and accessible wiring (`aria-invalid`,
 * `aria-describedby`). `ref` and every native input prop pass through to the <input>.
 */
export function TextField({
  label,
  error,
  helperText,
  containerClassName,
  leadingIcon,
  trailing,
  id,
  className,
  disabled,
  'aria-describedby': describedBy,
  'aria-invalid': ariaInvalid,
  ...rest
}: TextFieldProps) {
  const { controlId, hasError, messageId, ariaDescribedBy } = useFieldA11y(id, error, helperText, describedBy);
  return (
    <div className={cx('flex flex-col gap-1', containerClassName)}>
      {label !== undefined && <FieldLabel htmlFor={controlId}>{label}</FieldLabel>}
      <div className={fieldBoxClass(hasError, disabled)}>
        {leadingIcon && <Icon name={leadingIcon} className="text-on-surface-variant" />}
        <input
          {...rest}
          id={controlId}
          disabled={disabled}
          aria-invalid={hasError || ariaInvalid || undefined}
          aria-describedby={ariaDescribedBy}
          className={cx(
            'h-full min-w-0 flex-1 bg-transparent text-sm text-on-surface outline-none placeholder:text-muted',
            'focus-visible:outline-none',
            className,
          )}
        />
        {trailing}
      </div>
      {messageId && (
        <FieldMessage id={messageId} error={hasError}>
          {hasError ? error : helperText}
        </FieldMessage>
      )}
    </div>
  );
}
