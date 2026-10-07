/**
 * Scheduled-send time helpers [WP-F] (pure, unit-tested). Times are chosen in the user's
 * display timezone (Settings.timezone) — not the browser's — and sent as ms-epoch UTC.
 * Allowed range (DESIGN.md §1 B5): at least 1 minute and at most 30 days ahead.
 */
import { formatZhDateTime, safeTimeZone, zonedParts } from '@/lib/quote';

export const SCHEDULE_MIN_LEAD_MS = 60_000;
export const SCHEDULE_MAX_AHEAD_MS = 30 * 24 * 60 * 60_000;

/** Offset of `tz` from UTC at instant `ms` (local − UTC), in ms. */
export function tzOffsetMs(ms: number, tz: string): number {
  const whole = Math.floor(ms / 1000) * 1000;
  const p = zonedParts(whole, tz);
  return Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second) - whole;
}

/**
 * UTC instant of a wall-clock time in `tz`. In a DST gap the result is shifted forward by the
 * gap; in an overlap the earlier instant is used.
 */
export function zonedWallTimeToUtc(year: number, month: number, day: number, hour: number, minute: number, tz: string): number {
  const zone = safeTimeZone(tz);
  const wall = Date.UTC(year, month - 1, day, hour, minute);
  const first = wall - tzOffsetMs(wall, zone);
  const second = wall - tzOffsetMs(first, zone);
  for (const c of [Math.min(first, second), Math.max(first, second)]) {
    const p = zonedParts(c, zone);
    if (p.day === day && p.hour === hour && p.minute === minute) return c;
  }
  return Math.max(first, second); // inside a DST gap: the first valid instant after it
}

/** "GMT+08:00" for `tz` at `ms`. */
export function formatGmtOffset(ms: number, tz: string): string {
  const minutes = Math.round(tzOffsetMs(ms, tz) / 60_000);
  const sign = minutes < 0 ? '-' : '+';
  const abs = Math.abs(minutes);
  return `GMT${sign}${String(Math.floor(abs / 60)).padStart(2, '0')}:${String(abs % 60).padStart(2, '0')}`;
}

/** Calendar date `days` after the local date of `now` in `tz`. */
function localDatePlus(now: number, tz: string, days: number): { year: number; month: number; day: number; weekday: number } {
  const p = zonedParts(now, tz);
  const d = new Date(Date.UTC(p.year, p.month - 1, p.day + days));
  return { year: d.getUTCFullYear(), month: d.getUTCMonth() + 1, day: d.getUTCDate(), weekday: d.getUTCDay() };
}

export type SchedulePresetKey = 'tomorrowMorning' | 'tomorrowAfternoon' | 'mondayMorning';

export interface SchedulePreset {
  key: SchedulePresetKey;
  at: number;
}

/** 明天上午 8:00 · 明天下午 1:00 · 下周一上午 8:00 (the next Monday after today). */
export function schedulePresets(now: number, tz: string): SchedulePreset[] {
  const zone = safeTimeZone(tz);
  const tomorrow = localDatePlus(now, zone, 1);
  const today = zonedParts(now, zone).weekday;
  const toMonday = (1 - today + 7) % 7 || 7;
  const monday = localDatePlus(now, zone, toMonday);
  return [
    { key: 'tomorrowMorning', at: zonedWallTimeToUtc(tomorrow.year, tomorrow.month, tomorrow.day, 8, 0, zone) },
    { key: 'tomorrowAfternoon', at: zonedWallTimeToUtc(tomorrow.year, tomorrow.month, tomorrow.day, 13, 0, zone) },
    { key: 'mondayMorning', at: zonedWallTimeToUtc(monday.year, monday.month, monday.day, 8, 0, zone) },
  ];
}

export type ScheduleError = 'invalid' | 'too_soon' | 'too_late';

export function validateSchedule(at: number | null | undefined, now: number): ScheduleError | null {
  if (at === null || at === undefined || !Number.isFinite(at)) return 'invalid';
  if (at < now + SCHEDULE_MIN_LEAD_MS) return 'too_soon';
  if (at > now + SCHEDULE_MAX_AHEAD_MS) return 'too_late';
  return null;
}

const pad2 = (n: number) => String(n).padStart(2, '0');

/** `<input type=date|time>` values for `ms` in `tz`. */
export function toLocalInputs(ms: number, tz: string): { date: string; time: string } {
  const p = zonedParts(ms, tz);
  return { date: `${p.year}-${pad2(p.month)}-${pad2(p.day)}`, time: `${pad2(p.hour)}:${pad2(p.minute)}` };
}

/** Parses `YYYY-MM-DD` + `HH:MM` as a wall-clock time in `tz`; null when malformed. */
export function parseLocalInputs(date: string, time: string, tz: string): number | null {
  const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(date.trim());
  const tm = /^(\d{1,2}):(\d{2})$/.exec(time.trim());
  if (!d || !tm) return null;
  const [year, month, day, hour, minute] = [Number(d[1]), Number(d[2]), Number(d[3]), Number(tm[1]), Number(tm[2])];
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return null;
  const check = new Date(Date.UTC(year, month - 1, day));
  if (check.getUTCMonth() !== month - 1 || check.getUTCDate() !== day) return null; // e.g. 02-30
  return zonedWallTimeToUtc(year, month, day, hour, minute, tz);
}

/** "10月8日周四 08:00" (with the year when it differs from `now`'s). */
export function formatScheduleTime(ms: number, tz: string, now: number = Date.now()): string {
  const sameYear = zonedParts(ms, tz).year === zonedParts(now, tz).year;
  return formatZhDateTime(ms, tz, { omitYear: sameYear });
}
