import { useId, type ComponentProps, type ReactNode } from 'react';
import { cx } from './cx';

export interface SwitchProps extends Omit<ComponentProps<'button'>, 'onChange' | 'children' | 'value' | 'role'> {
  checked: boolean;
  onCheckedChange: (checked: boolean) => void;
  /** Visible label (after the switch). Without it, pass `aria-label`. */
  label?: ReactNode;
  /** Wrapper class when `label` is given. */
  labelClassName?: string;
}

/** Material 3 switch: a `<button role="switch" aria-checked>`; Space / Enter toggle it. */
export function Switch({
  checked,
  onCheckedChange,
  label,
  labelClassName,
  className,
  id,
  disabled,
  onClick,
  type = 'button',
  ...rest
}: SwitchProps) {
  const autoId = useId();
  const switchId = id ?? autoId;
  const control = (
    <button
      {...rest}
      id={switchId}
      type={type}
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      data-state={checked ? 'checked' : 'unchecked'}
      onClick={(e) => {
        onClick?.(e);
        if (!e.defaultPrevented) onCheckedChange(!checked);
      }}
      className={cx(
        'relative inline-flex h-8 w-[52px] shrink-0 items-center rounded-full border-2 transition-colors duration-150',
        checked ? 'border-primary bg-primary' : 'border-outline bg-surface-container-highest',
        'disabled:pointer-events-none disabled:opacity-40',
        className,
      )}
    >
      <span
        aria-hidden="true"
        className={cx(
          'block rounded-full transition-[transform,width,height,background-color] duration-150',
          checked ? 'size-6 translate-x-[22px] bg-on-primary' : 'size-4 translate-x-[6px] bg-outline',
        )}
      />
    </button>
  );
  if (label === undefined) return control;
  return (
    <span className={cx('inline-flex items-center gap-3', labelClassName)}>
      {control}
      <label htmlFor={switchId} className="cursor-pointer select-none text-sm text-on-surface">
        {label}
      </label>
    </span>
  );
}
