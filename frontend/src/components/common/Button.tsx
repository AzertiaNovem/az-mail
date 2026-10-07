import type { ComponentProps } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';
import { Spinner } from './Spinner';

export type ButtonVariant = 'primary' | 'tonal' | 'text' | 'outlined';
export type ButtonSize = 'sm' | 'md' | 'lg';

export interface ButtonProps extends ComponentProps<'button'> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  /** 'pill' (default) or 'rounded' (16 px corners, like Gmail's 写邮件 FAB). */
  shape?: 'pill' | 'rounded';
  /** Leading Material Symbol. */
  icon?: string;
  trailingIcon?: string;
  /** Shows a spinner in place of the leading icon and disables the button. */
  loading?: boolean;
  /** Destructive styling (error color) for any variant. */
  danger?: boolean;
}

const variants: Record<ButtonVariant, string> = {
  primary: 'bg-primary text-on-primary hover:bg-primary-hover hover:shadow-elevation-1 active:shadow-none',
  tonal:
    'bg-secondary-container text-on-secondary-container hover:bg-compose-hover hover:shadow-elevation-1 active:shadow-none',
  text: 'bg-transparent text-primary hover:bg-primary/8 active:bg-primary/12',
  outlined:
    'border border-outline bg-transparent text-on-surface-variant hover:bg-on-surface-variant/8 active:bg-on-surface-variant/12',
};

const dangerVariants: Record<ButtonVariant, string> = {
  primary: 'bg-error text-white hover:bg-error/90 hover:shadow-elevation-1 active:shadow-none',
  tonal: 'bg-error-container text-on-error-container hover:shadow-elevation-1 active:shadow-none',
  text: 'bg-transparent text-error hover:bg-error/8 active:bg-error/12',
  outlined: 'border border-outline bg-transparent text-error hover:bg-error/8 active:bg-error/12',
};

const sizes: Record<ButtonSize, string> = {
  sm: 'h-8 px-3 text-[13px] gap-1.5',
  md: 'h-9 px-4 text-sm gap-2',
  lg: 'h-14 px-5 text-sm gap-3',
};

const iconSizes: Record<ButtonSize, number> = { sm: 18, md: 18, lg: 24 };

/** Material 3 button (Gmail flavour). Defaults to type="button". */
export function Button({
  variant = 'primary',
  size = 'md',
  shape = 'pill',
  icon,
  trailingIcon,
  loading = false,
  danger = false,
  disabled,
  className,
  children,
  type = 'button',
  ...rest
}: ButtonProps) {
  const iconSize = iconSizes[size];
  return (
    <button
      {...rest}
      type={type}
      disabled={disabled || loading}
      aria-busy={loading || undefined}
      className={cx(
        'inline-flex shrink-0 select-none items-center justify-center whitespace-nowrap font-medium tracking-[0.01em]',
        'transition-[background-color,box-shadow,color] duration-150',
        'disabled:pointer-events-none disabled:opacity-40',
        shape === 'pill' ? 'rounded-full' : 'rounded-2xl',
        sizes[size],
        (danger ? dangerVariants : variants)[variant],
        className,
      )}
    >
      {loading ? (
        <Spinner size={iconSize} inheritColor decorative />
      ) : icon ? (
        <Icon name={icon} size={iconSize} />
      ) : null}
      {children}
      {trailingIcon && <Icon name={trailingIcon} size={iconSize} />}
    </button>
  );
}
