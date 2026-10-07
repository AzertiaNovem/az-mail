import type { ComponentProps } from 'react';
import { cx } from './cx';
import { Icon } from './Icon';
import { Tooltip } from './Tooltip';

export type IconButtonSize = 'sm' | 'md' | 'lg';

export interface IconButtonProps extends Omit<ComponentProps<'button'>, 'children'> {
  /** Material Symbol name. */
  icon: string;
  /** Accessible name; also the tooltip text unless `tooltip` overrides it. */
  label: string;
  size?: IconButtonSize;
  /** Override the glyph size (px). */
  iconSize?: number;
  /** Filled glyph (e.g. a set star). */
  fill?: boolean;
  /** Toggle state → aria-pressed + selected styling. */
  active?: boolean;
  /** false = no tooltip; a string = tooltip text different from `label`. */
  tooltip?: boolean | string;
  tooltipSide?: 'top' | 'right' | 'bottom' | 'left';
  /** Shortcut hint shown in the tooltip. */
  shortcut?: string;
  /** 'inverse' for dark surfaces (toasts). */
  tone?: 'default' | 'primary' | 'inverse';
}

const boxes: Record<IconButtonSize, string> = { sm: 'size-8', md: 'size-10', lg: 'size-12' };
const glyphs: Record<IconButtonSize, number> = { sm: 18, md: 20, lg: 24 };
const tones = {
  default: 'text-on-surface-variant hover:bg-on-surface/8 active:bg-on-surface/12 focus-visible:bg-on-surface/12',
  primary: 'text-primary hover:bg-primary/8 active:bg-primary/12 focus-visible:bg-primary/12',
  inverse: 'text-inverse-on-surface hover:bg-white/10 active:bg-white/16 focus-visible:bg-white/16',
} as const;

/** Round icon-only button with Gmail's circular hover state and a tooltip. */
export function IconButton({
  icon,
  label,
  size = 'md',
  iconSize,
  fill,
  active,
  tooltip = true,
  tooltipSide,
  shortcut,
  tone = 'default',
  className,
  type = 'button',
  ...rest
}: IconButtonProps) {
  const button = (
    <button
      {...rest}
      type={type}
      aria-label={label}
      aria-pressed={active === undefined ? undefined : active}
      className={cx(
        'inline-flex shrink-0 items-center justify-center rounded-full transition-colors duration-150',
        'disabled:pointer-events-none disabled:opacity-40',
        boxes[size],
        tones[tone],
        active && tone === 'default' && 'bg-on-surface/8 text-on-surface',
        className,
      )}
    >
      <Icon name={icon} size={iconSize ?? glyphs[size]} fill={fill} />
    </button>
  );
  if (tooltip === false) return button;
  return (
    <Tooltip label={typeof tooltip === 'string' ? tooltip : label} side={tooltipSide} shortcut={shortcut}>
      {button}
    </Tooltip>
  );
}
