/**
 * Label colors [WP-E]. Labels store one hex color (`Label.color`, default #9aa0a6); chips are
 * drawn as a light tint of it with dark, readable text (Gmail style), and the sidebar shows the
 * color itself as a dot.
 */

/** Picker palette (Gmail-like label colors), in display order. */
export const LABEL_COLORS = [
  '#9aa0a6',
  '#5f6368',
  '#d93025',
  '#e8710a',
  '#f9ab00',
  '#188038',
  '#12b5cb',
  '#1a73e8',
  '#9334e6',
  '#e52592',
  '#a142f4',
  '#795548',
] as const;

export const DEFAULT_LABEL_COLOR = '#9aa0a6';

const HEX_RE = /^#(?:[0-9a-f]{3}|[0-9a-f]{6})$/i;

export function isHexColor(v: string | null | undefined): v is string {
  return typeof v === 'string' && HEX_RE.test(v.trim());
}

interface Rgb {
  r: number;
  g: number;
  b: number;
}

/** Parses #rgb / #rrggbb; invalid input → the default label gray. */
export function parseHex(color: string | null | undefined): Rgb {
  let hex = isHexColor(color) ? color.trim().slice(1) : DEFAULT_LABEL_COLOR.slice(1);
  if (hex.length === 3) hex = hex.replace(/(.)/g, '$1$1');
  return {
    r: Number.parseInt(hex.slice(0, 2), 16),
    g: Number.parseInt(hex.slice(2, 4), 16),
    b: Number.parseInt(hex.slice(4, 6), 16),
  };
}

export function toHex({ r, g, b }: Rgb): string {
  const h = (n: number) =>
    Math.round(Math.min(255, Math.max(0, n)))
      .toString(16)
      .padStart(2, '0');
  return `#${h(r)}${h(g)}${h(b)}`;
}

/** Mixes `color` with `other` (`amount` 0 = color, 1 = other). */
export function mix(color: string, other: string, amount: number): string {
  const a = parseHex(color);
  const b = parseHex(other);
  const t = Math.min(1, Math.max(0, amount));
  return toHex({ r: a.r + (b.r - a.r) * t, g: a.g + (b.g - a.g) * t, b: a.b + (b.b - a.b) * t });
}

/** WCAG relative luminance (0..1). */
export function luminance(color: string): number {
  const { r, g, b } = parseHex(color);
  const lin = (c: number) => {
    const s = c / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

/** WCAG contrast ratio between two colors (1..21). */
export function contrastRatio(a: string, b: string): number {
  const la = luminance(a);
  const lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

/** Black or white, whichever reads better on `background`. */
export function readableTextColor(background: string): '#000000' | '#ffffff' {
  return contrastRatio(background, '#000000') >= contrastRatio(background, '#ffffff') ? '#000000' : '#ffffff';
}

/** Chip colors for a label: a light tint background with a darkened, readable foreground. */
export function labelChipColors(color: string | null | undefined): { background: string; foreground: string } {
  const base = isHexColor(color) ? color : DEFAULT_LABEL_COLOR;
  const background = mix(base, '#ffffff', 0.78);
  let foreground = mix(base, '#000000', 0.45);
  // Very light bases (yellow) need more darkening to stay readable on their tint.
  for (let k = 0.55; contrastRatio(foreground, background) < 4.5 && k <= 0.95; k += 0.1) {
    foreground = mix(base, '#000000', k);
  }
  return { background, foreground };
}
