/** Admin table formatting [WP-F]: dates in the admin's display timezone, bytes, counts. */
import { zonedParts } from '@/lib/quote';

export { formatBytes } from '@/lib/upload';

const pad2 = (n: number) => String(n).padStart(2, '0');

/** "2026-10-07 20:03" (":15" with `seconds`); '' for null. */
export function formatDateTime(ms: number | null | undefined, tz: string, opts: { seconds?: boolean } = {}): string {
  if (ms === null || ms === undefined || !Number.isFinite(ms)) return '';
  const p = zonedParts(ms, tz);
  const base = `${p.year}-${pad2(p.month)}-${pad2(p.day)} ${pad2(p.hour)}:${pad2(p.minute)}`;
  return opts.seconds ? `${base}:${pad2(p.second)}` : base;
}

/** "1,234" */
export const formatCount = (n: number): string => (Number.isFinite(n) ? n.toLocaleString('zh-CN') : '0');
