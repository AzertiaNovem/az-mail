import * as CB from '@radix-ui/react-checkbox';
import { useId, type ComponentProps, type ReactNode } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';

export interface CheckboxProps
  extends Omit<ComponentProps<typeof CB.Root>, 'checked' | 'defaultChecked' | 'onCheckedChange' | 'children'> {
  checked: boolean | 'indeterminate';
  onCheckedChange?: (checked: boolean) => void;
  /** Visible label. Without it, pass `aria-label`. */
  label?: ReactNode;
  /** Wrapper class when `label` is given. */
  labelClassName?: string;
}

/** Material checkbox (18 px box, tri-state). */
export function Checkbox({ checked, onCheckedChange, label, labelClassName, className, id, ...rest }: CheckboxProps) {
  const autoId = useId();
  const boxId = id ?? autoId;
  const box = (
    <CB.Root
      {...rest}
      id={boxId}
      checked={checked}
      onCheckedChange={(c) => onCheckedChange?.(c === true)}
      className={cx(
        'inline-flex size-[18px] shrink-0 items-center justify-center rounded-[2px] border-2 border-on-surface-variant',
        'bg-transparent text-on-primary transition-colors',
        'data-[state=checked]:border-primary data-[state=checked]:bg-primary',
        'data-[state=indeterminate]:border-primary data-[state=indeterminate]:bg-primary',
        'disabled:pointer-events-none disabled:opacity-40',
        className,
      )}
    >
      <CB.Indicator className="flex items-center justify-center">
        <Icon name={checked === 'indeterminate' ? 'remove' : 'check'} size={16} weight={700} />
      </CB.Indicator>
    </CB.Root>
  );
  if (label === undefined) return box;
  return (
    <span className={cx('inline-flex items-center gap-3', labelClassName)}>
      {box}
      <label htmlFor={boxId} className="cursor-pointer select-none text-sm text-on-surface">
        {label}
      </label>
    </span>
  );
}
