import type { CSSProperties, HTMLAttributes } from 'react';
import { cx } from './cx';

export interface IconProps extends Omit<HTMLAttributes<HTMLSpanElement>, 'children'> {
  /** Material Symbols ligature name, e.g. "inbox", "star", "attach_file". */
  name: string;
  /** Pixel size (font-size). Default 20 (Gmail toolbar size). */
  size?: number;
  /** Filled variant (FILL axis = 1), e.g. a set star. */
  fill?: boolean;
  /** Weight axis (100–700). Default 400. */
  weight?: 100 | 200 | 300 | 400 | 500 | 600 | 700;
  /** Accessible label. Without it the icon is decorative (aria-hidden). */
  label?: string;
}

/** Self-hosted Material Symbols Outlined icon. */
export function Icon({ name, size = 20, fill = false, weight, label, className, style, ...rest }: IconProps) {
  const vars = {
    fontSize: size,
    '--icon-fill': fill ? 1 : 0,
    '--icon-opsz': Math.min(48, Math.max(20, size)),
    ...(weight ? { '--icon-wght': weight } : null),
    ...style,
  } as CSSProperties;
  return (
    <span
      {...rest}
      className={cx('material-symbols-outlined', className)}
      style={vars}
      role={label ? 'img' : undefined}
      aria-label={label}
      aria-hidden={label ? undefined : true}
      translate="no"
    >
      {name}
    </span>
  );
}
