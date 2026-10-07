/**
 * zh-CN date / size formatting for the mail UI [WP-E].
 *
 * Every function takes the display time zone explicitly (`Settings.timezone`, default
 * Asia/Shanghai; DESIGN §1 C9 "used for display only"). An unknown zone falls back to the
 * browser's local zone instead of throwing. Times are ms-epoch UTC.
 *
 *   list dates   today → "14:05"; this year → "10月7日"; older → "2025/3/9"
 *   full date    "2026年10月7日 周三 20:03"
 *   relative     "刚刚" / "5 分钟前" / "3 小时前" / "2 天前"
 */

const DEFAULT_TZ = 'Asia/Shanghai';
const DAY_MS = 86_400_000;

interface DateParts {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  /** "周三" */
  weekday: string;
}

const partsFormatters = new Map<string, Intl.DateTimeFormat>();
const validZones = new Map<string, boolean>();

/** Whether `tz` is an IANA zone this runtime knows. */
export function isValidTimeZone(tz: string | null | undefined): tz is string {
  if (!tz) return false;
  const cached = validZones.get(tz);
  if (cached !== undefined) return cached;
  let ok = true;
  try {
    new Intl.DateTimeFormat('en-US', { timeZone: tz });
  } catch {
    ok = false;
  }
  validZones.set(tz, ok);
  return ok;
}

/** The zone actually used: `tz` if valid, else the runtime's local zone (undefined). */
function zone(tz: string | null | undefined): string | undefined {
  return isValidTimeZone(tz) ? tz : undefined;
}

function partsFormatter(tz: string | undefined): Intl.DateTimeFormat {
  const key = tz ?? '';
  let f = partsFormatters.get(key);
  if (!f) {
    f = new Intl.DateTimeFormat('zh-CN', {
      timeZone: tz,
      year: 'numeric',
      month: 'numeric',
      day: 'numeric',
      hour: 'numeric',
      minute: 'numeric',
      weekday: 'short',
      hourCycle: 'h23',
    });
    partsFormatters.set(key, f);
  }
  return f;
}

/** Calendar fields of `ts` in `tz`. */
export function dateParts(ts: number, tz?: string | null): DateParts {
  const out: DateParts = { year: 0, month: 0, day: 0, hour: 0, minute: 0, weekday: '' };
  for (const p of partsFormatter(zone(tz)).formatToParts(new Date(ts))) {
    switch (p.type) {
      case 'year':
        out.year = Number(p.value);
        break;
      case 'month':
        out.month = Number(p.value);
        break;
      case 'day':
        out.day = Number(p.value);
        break;
      case 'hour':
        // Some engines render midnight as "24" with h23; normalize.
        out.hour = Number(p.value) % 24;
        break;
      case 'minute':
        out.minute = Number(p.value);
        break;
      case 'weekday':
        out.weekday = p.value;
        break;
      default:
        break;
    }
  }
  return out;
}

const pad2 = (n: number): string => String(n).padStart(2, '0');

/** "14:05" */
export function formatTime(ts: number, tz?: string | null): string {
  const p = dateParts(ts, tz);
  return `${pad2(p.hour)}:${pad2(p.minute)}`;
}

function sameDay(a: DateParts, b: DateParts): boolean {
  return a.year === b.year && a.month === b.month && a.day === b.day;
}

/** Day number (days since epoch) of a calendar date, for day differences across zones. */
function dayIndex(p: DateParts): number {
  return Math.floor(Date.UTC(p.year, p.month - 1, p.day) / DAY_MS);
}

/** Thread-list date column: today → "HH:mm"; this year → "M月D日"; else "YYYY/M/D". */
export function formatListDate(ts: number, now: number = Date.now(), tz?: string | null): string {
  const p = dateParts(ts, tz);
  const n = dateParts(now, tz);
  if (sameDay(p, n)) return `${pad2(p.hour)}:${pad2(p.minute)}`;
  if (p.year === n.year) return `${p.month}月${p.day}日`;
  return `${p.year}/${p.month}/${p.day}`;
}

/** "2026年10月7日 周三 20:03" */
export function formatFullDate(ts: number, tz?: string | null): string {
  const p = dateParts(ts, tz);
  return `${p.year}年${p.month}月${p.day}日 ${p.weekday} ${pad2(p.hour)}:${pad2(p.minute)}`;
}

/** Message header date: today → "20:03"; this year → "10月5日 周一 15:20"; else full date. */
export function formatMessageDate(ts: number, now: number = Date.now(), tz?: string | null): string {
  const p = dateParts(ts, tz);
  const n = dateParts(now, tz);
  if (sameDay(p, n)) return `${pad2(p.hour)}:${pad2(p.minute)}`;
  if (p.year === n.year) return `${p.month}月${p.day}日 ${p.weekday} ${pad2(p.hour)}:${pad2(p.minute)}`;
  return formatFullDate(ts, tz);
}

/**
 * Relative age for recent times ("刚刚", "5 分钟前", "3 小时前", "2 天前"); null when older
 * than `maxDays` or in the future by more than a minute.
 */
export function formatRelative(ts: number, now: number = Date.now(), maxDays = 30): string | null {
  const diff = now - ts;
  if (diff < -60_000) return null;
  if (diff < 60_000) return '刚刚';
  const minutes = Math.floor(diff / 60_000);
  if (minutes < 60) return `${minutes} 分钟前`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours} 小时前`;
  const days = Math.floor(hours / 24);
  if (days <= maxDays) return `${days} 天前`;
  return null;
}

/**
 * Upcoming time (scheduled sends): "今天 09:00", "明天 09:00", "10月8日 09:00", "2027年1月2日 09:00".
 */
export function formatScheduleTime(ts: number, now: number = Date.now(), tz?: string | null): string {
  const p = dateParts(ts, tz);
  const n = dateParts(now, tz);
  const time = `${pad2(p.hour)}:${pad2(p.minute)}`;
  const delta = dayIndex(p) - dayIndex(n);
  if (delta === 0) return `今天 ${time}`;
  if (delta === 1) return `明天 ${time}`;
  if (p.year === n.year) return `${p.month}月${p.day}日 ${time}`;
  return `${p.year}年${p.month}月${p.day}日 ${time}`;
}

/** Calendar date "YYYY/MM/DD" in `tz` (the search grammar's date form). */
export function formatSearchDate(ts: number, tz?: string | null): string {
  const p = dateParts(ts, tz);
  return `${p.year}/${pad2(p.month)}/${pad2(p.day)}`;
}

/** File sizes: "512 B", "12 KB", "1.5 MB", "2.1 GB" (binary units, like Gmail). */
export function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n < 0) return '';
  if (n < 1024) return `${Math.round(n)} B`;
  const kb = n / 1024;
  if (kb < 1024) return `${Math.max(1, Math.round(kb))} KB`;
  const mb = kb / 1024;
  if (mb < 1024) return `${mb < 10 ? mb.toFixed(1).replace(/\.0$/, '') : Math.round(mb)} MB`;
  const gb = mb / 1024;
  return `${gb < 10 ? gb.toFixed(1).replace(/\.0$/, '') : Math.round(gb)} GB`;
}

/** Thousands separators ("1,234"). */
export function formatCount(n: number): string {
  return n.toLocaleString('zh-CN');
}

export { DEFAULT_TZ };
