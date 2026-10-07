import type { ComponentProps } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';
import { FieldLabel, FieldMessage, fieldBoxClass, useFieldA11y, type FieldChromeProps } from './TextField';

export interface SelectOption<V extends string = string> {
  value: V;
  label: string;
  disabled?: boolean;
}

export interface SelectProps<V extends string = string>
  extends Omit<ComponentProps<'select'>, 'value' | 'defaultValue' | 'onChange' | 'children' | 'multiple'>,
    FieldChromeProps {
  options: readonly SelectOption<V>[];
  value: V;
  onValueChange: (value: V) => void;
}

/**
 * Styled native <select> (keyboard and screen-reader behaviour for free), with the same label /
 * helper / error chrome as TextField. Values are strings; map numbers (ids, seconds) yourself.
 */
export function Select<V extends string = string>({
  options,
  value,
  onValueChange,
  label,
  error,
  helperText,
  containerClassName,
  id,
  className,
  disabled,
  'aria-describedby': describedBy,
  ...rest
}: SelectProps<V>) {
  const { controlId, hasError, messageId, ariaDescribedBy } = useFieldA11y(id, error, helperText, describedBy);
  return (
    <div className={cx('flex flex-col gap-1', containerClassName)}>
      {label !== undefined && <FieldLabel htmlFor={controlId}>{label}</FieldLabel>}
      <div className={cx(fieldBoxClass(hasError, disabled), 'relative pr-0')}>
        <select
          {...rest}
          id={controlId}
          value={value}
          disabled={disabled}
          aria-invalid={hasError || undefined}
          aria-describedby={ariaDescribedBy}
          onChange={(e) => onValueChange(e.target.value as V)}
          className={cx(
            'h-full min-w-0 flex-1 cursor-pointer appearance-none bg-transparent pr-9 text-sm text-on-surface outline-none',
            'focus-visible:outline-none',
            className,
          )}
        >
          {options.map((o) => (
            <option key={o.value} value={o.value} disabled={o.disabled}>
              {o.label}
            </option>
          ))}
        </select>
        <Icon name="arrow_drop_down" size={24} className="pointer-events-none absolute right-2 text-on-surface-variant" />
      </div>
      {messageId && (
        <FieldMessage id={messageId} error={hasError}>
          {hasError ? error : helperText}
        </FieldMessage>
      )}
    </div>
  );
}
