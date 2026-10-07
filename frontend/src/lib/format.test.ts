import { describe, expect, it } from 'vitest';
import {
  dateParts,
  formatBytes,
  formatCount,
  formatFullDate,
  formatListDate,
  formatMessageDate,
  formatRelative,
  formatScheduleTime,
  formatSearchDate,
  formatTime,
  isValidTimeZone,
} from './format';

const SH = 'Asia/Shanghai';
/** 2026-10-07 20:03 in Shanghai (UTC+8) = 12:03 UTC. A Wednesday. */
const NOW = Date.UTC(2026, 9, 7, 12, 3);
const H = 3_600_000;
const D = 24 * H;

describe('zh-CN date formatting', () => {
  it('list dates: today → HH:mm, this year → M月D日, else YYYY/M/D', () => {
    expect(formatListDate(NOW - 2 * H, NOW, SH)).toBe('18:03');
    expect(formatListDate(Date.UTC(2026, 9, 6, 16, 0), NOW, SH)).toBe('00:00'); // 00:00 on the 7th in Shanghai
    expect(formatListDate(Date.UTC(2026, 9, 6, 15, 59), NOW, SH)).toBe('10月6日');
    expect(formatListDate(Date.UTC(2026, 0, 2, 4, 0), NOW, SH)).toBe('1月2日');
    expect(formatListDate(Date.UTC(2025, 11, 31, 15, 0), NOW, SH)).toBe('2025/12/31');
    // The same instant is "today" in Shanghai but yesterday in New York.
    expect(formatListDate(Date.UTC(2026, 9, 7, 2, 0), NOW, 'America/New_York')).toBe('10月6日');
  });

  it('full, message and time formats', () => {
    expect(formatFullDate(NOW, SH)).toBe('2026年10月7日 周三 20:03');
    expect(formatTime(Date.UTC(2026, 9, 7, 1, 5), SH)).toBe('09:05');
    expect(formatMessageDate(NOW - H, NOW, SH)).toBe('19:03');
    expect(formatMessageDate(NOW - 2 * D, NOW, SH)).toBe('10月5日 周一 20:03');
    expect(formatMessageDate(Date.UTC(2024, 1, 29, 4, 0), NOW, SH)).toBe('2024年2月29日 周四 12:00');
    expect(formatSearchDate(NOW, SH)).toBe('2026/10/07');
  });

  it('relative ages', () => {
    expect(formatRelative(NOW - 10_000, NOW)).toBe('刚刚');
    expect(formatRelative(NOW - 5 * 60_000, NOW)).toBe('5 分钟前');
    expect(formatRelative(NOW - 3 * H, NOW)).toBe('3 小时前');
    expect(formatRelative(NOW - 2 * D, NOW)).toBe('2 天前');
    expect(formatRelative(NOW - 40 * D, NOW)).toBeNull();
    expect(formatRelative(NOW + 5 * 60_000, NOW)).toBeNull();
  });

  it('schedule times', () => {
    expect(formatScheduleTime(NOW + H, NOW, SH)).toBe('今天 21:03');
    expect(formatScheduleTime(Date.UTC(2026, 9, 8, 1, 0), NOW, SH)).toBe('明天 09:00');
    expect(formatScheduleTime(Date.UTC(2026, 9, 20, 1, 0), NOW, SH)).toBe('10月20日 09:00');
    expect(formatScheduleTime(Date.UTC(2027, 0, 2, 1, 0), NOW, SH)).toBe('2027年1月2日 09:00');
  });

  it('falls back to the local zone for unknown zones', () => {
    expect(isValidTimeZone('Asia/Shanghai')).toBe(true);
    expect(isValidTimeZone('Mars/Olympus')).toBe(false);
    expect(isValidTimeZone('')).toBe(false);
    expect(() => formatListDate(NOW, NOW, 'Mars/Olympus')).not.toThrow();
    expect(dateParts(NOW, 'Mars/Olympus').year).toBe(2026);
  });
});

describe('sizes and counts', () => {
  it('formats bytes with binary units', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(1023)).toBe('1023 B');
    expect(formatBytes(1024)).toBe('1 KB');
    expect(formatBytes(1536)).toBe('2 KB');
    expect(formatBytes(1.5 * 1024 * 1024)).toBe('1.5 MB');
    expect(formatBytes(2 * 1024 * 1024)).toBe('2 MB');
    expect(formatBytes(25 * 1024 * 1024)).toBe('25 MB');
    expect(formatBytes(3.2 * 1024 ** 3)).toBe('3.2 GB');
    expect(formatBytes(-1)).toBe('');
    expect(formatBytes(Number.NaN)).toBe('');
  });

  it('groups thousands', () => {
    expect(formatCount(1234567)).toBe('1,234,567');
    expect(formatCount(12)).toBe('12');
  });
});
