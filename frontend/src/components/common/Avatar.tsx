import { cx } from './cx';

/** Accessible (white-text) avatar colors in Google's palette. */
export const AVATAR_COLORS = [
  '#1a73e8',
  '#c5221f',
  '#137333',
  '#b06000',
  '#8430ce',
  '#007b83',
  '#b80672',
  '#5f6368',
  '#185abc',
  '#a50e0e',
  '#0d652d',
  '#7627bb',
] as const;

/** FNV-1a 32-bit hash. */
function hash(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

/** Deterministic color for an address (case-insensitive). */
export function avatarColor(email: string): string {
  const key = email.trim().toLowerCase();
  return AVATAR_COLORS[hash(key) % AVATAR_COLORS.length] ?? AVATAR_COLORS[0];
}

/** First character of the display name (or the email's local part), upper-cased. */
export function avatarInitial(name: string | null | undefined, email: string): string {
  const source = (name ?? '').replace(/^["'\s(<]+/, '').trim() || email.trim();
  const first = Array.from(source)[0] ?? '?';
  return first.toLocaleUpperCase('zh-CN');
}

export interface AvatarProps {
  name?: string | null;
  email: string;
  /** Pixel size. Default 32. */
  size?: number;
  className?: string;
  /** Hide from assistive tech when the name is shown next to it. */
  decorative?: boolean;
}

/** Round initial avatar (no remote images). */
export function Avatar({ name, email, size = 32, className, decorative = false }: AvatarProps) {
  const label = name?.trim() || email;
  return (
    <span
      role={decorative ? undefined : 'img'}
      aria-label={decorative ? undefined : label}
      aria-hidden={decorative || undefined}
      title={decorative ? undefined : label}
      className={cx(
        'inline-flex shrink-0 select-none items-center justify-center rounded-full font-medium leading-none text-white',
        className,
      )}
      style={{ width: size, height: size, fontSize: Math.round(size * 0.45), backgroundColor: avatarColor(email) }}
    >
      {avatarInitial(name, email)}
    </span>
  );
}
