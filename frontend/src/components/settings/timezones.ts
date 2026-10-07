/** Time zone choices for the settings page [WP-F]: common zones, labelled "(GMT+08:00) 北京、上海". */
import { formatGmtOffset, tzOffsetMs } from '@/components/compose/schedule';
import { zh } from '@/i18n/zh';

export const COMMON_TIMEZONES = Object.keys(zh.settings.zones) as (keyof typeof zh.settings.zones)[];

export interface TimezoneOption {
  value: string;
  label: string;
}

/** Options sorted by UTC offset; `current` is included even when it is not a common zone. */
export function timezoneOptions(current: string, now: number = Date.now()): TimezoneOption[] {
  const zones: string[] = [...COMMON_TIMEZONES];
  if (current && !zones.includes(current)) {
    try {
      new Intl.DateTimeFormat('en-US', { timeZone: current });
      zones.push(current);
    } catch {
      /* invalid zone: not offered */
    }
  }
  const names = zh.settings.zones as Record<string, string>;
  return zones
    .map((z) => ({ z, offset: tzOffsetMs(now, z) }))
    .sort((a, b) => a.offset - b.offset || a.z.localeCompare(b.z))
    .map(({ z }) => ({ value: z, label: `(${formatGmtOffset(now, z)}) ${names[z] ?? z}` }));
}
