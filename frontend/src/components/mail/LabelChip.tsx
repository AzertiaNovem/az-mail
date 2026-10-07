import type { MouseEvent } from 'react';
import { cx, Icon } from '@/components/common';
import { t } from '@/i18n/zh';
import { labelChipColors } from '@/lib/color';

export interface LabelChipProps {
  name: string;
  /** Label color; omit for the neutral folder chip (e.g. "收件箱"). */
  color?: string;
  /** Shows a × that calls this (e.g. remove the label from the thread). */
  onRemove?: () => void;
  removeLabel?: string;
  size?: 'sm' | 'md';
  className?: string;
}

/** Gmail label chip: light tint of the label color with dark text; optional remove button. */
export function LabelChip({ name, color, onRemove, removeLabel, size = 'sm', className }: LabelChipProps) {
  const colors = color ? labelChipColors(color) : { background: '#e8eaed', foreground: '#3c4043' };
  const stop = (e: MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    onRemove?.();
  };
  return (
    <span
      className={cx(
        'inline-flex max-w-[160px] shrink-0 items-center rounded font-normal leading-none',
        size === 'sm' ? 'h-[18px] px-1 text-xs' : 'h-6 gap-0.5 px-1.5 text-[13px]',
        className,
      )}
      style={{ backgroundColor: colors.background, color: colors.foreground }}
      title={name}
    >
      <span className="truncate">{name}</span>
      {onRemove && (
        <button
          type="button"
          onClick={stop}
          aria-label={removeLabel ?? t('mail.thread.removeLabel', { name })}
          className="-mr-0.5 ml-0.5 inline-flex size-4 items-center justify-center rounded-sm opacity-70 hover:bg-black/10 hover:opacity-100"
        >
          <Icon name="close" size={14} />
        </button>
      )}
    </span>
  );
}
