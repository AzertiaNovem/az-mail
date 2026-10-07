import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { SchedulePicker } from './SchedulePicker';
import {
  formatGmtOffset,
  formatScheduleTime,
  parseLocalInputs,
  SCHEDULE_MAX_AHEAD_MS,
  SCHEDULE_MIN_LEAD_MS,
  schedulePresets,
  toLocalInputs,
  tzOffsetMs,
  validateSchedule,
  zonedWallTimeToUtc,
} from './schedule';

const SH = 'Asia/Shanghai';
// Wednesday 2026-10-07 20:03 in Shanghai.
const NOW = Date.UTC(2026, 9, 7, 12, 3);

describe('schedule helpers', () => {
  it('converts wall-clock times in a zone to UTC', () => {
    expect(zonedWallTimeToUtc(2026, 10, 8, 8, 0, SH)).toBe(Date.UTC(2026, 9, 8, 0, 0));
    expect(zonedWallTimeToUtc(2026, 7, 1, 9, 30, 'America/New_York')).toBe(Date.UTC(2026, 6, 1, 13, 30)); // EDT
    expect(zonedWallTimeToUtc(2026, 1, 15, 9, 30, 'America/New_York')).toBe(Date.UTC(2026, 0, 15, 14, 30)); // EST
    // 2026-03-08 02:30 does not exist in New York (spring forward): moved past the gap.
    const gap = zonedWallTimeToUtc(2026, 3, 8, 2, 30, 'America/New_York');
    expect(gap).toBeGreaterThanOrEqual(Date.UTC(2026, 2, 8, 7, 0));
    expect(tzOffsetMs(NOW, SH)).toBe(8 * 3600_000);
    expect(formatGmtOffset(NOW, SH)).toBe('GMT+08:00');
    expect(formatGmtOffset(NOW, 'Asia/Kolkata')).toBe('GMT+05:30');
    expect(formatGmtOffset(NOW, 'America/Los_Angeles')).toBe('GMT-07:00');
  });

  it('computes the three presets in the user timezone', () => {
    const [tm, ta, mon] = schedulePresets(NOW, SH);
    expect(tm).toEqual({ key: 'tomorrowMorning', at: Date.UTC(2026, 9, 8, 0, 0) }); // Thu 08:00
    expect(ta).toEqual({ key: 'tomorrowAfternoon', at: Date.UTC(2026, 9, 8, 5, 0) }); // Thu 13:00
    expect(mon).toEqual({ key: 'mondayMorning', at: Date.UTC(2026, 9, 12, 0, 0) }); // Mon 12th 08:00
    // On a Monday, "下周一" is next week's Monday.
    const monday = Date.UTC(2026, 9, 12, 2, 0); // Mon 10:00 Shanghai
    expect(schedulePresets(monday, SH)[2]!.at).toBe(Date.UTC(2026, 9, 19, 0, 0));
    // On a Sunday it is the next day.
    const sunday = Date.UTC(2026, 9, 11, 2, 0);
    expect(schedulePresets(sunday, SH)[2]!.at).toBe(Date.UTC(2026, 9, 12, 0, 0));
  });

  it('validates 1 minute – 30 days ahead', () => {
    expect(validateSchedule(NOW + SCHEDULE_MIN_LEAD_MS, NOW)).toBeNull();
    expect(validateSchedule(NOW + SCHEDULE_MIN_LEAD_MS - 1, NOW)).toBe('too_soon');
    expect(validateSchedule(NOW - 1000, NOW)).toBe('too_soon');
    expect(validateSchedule(NOW + SCHEDULE_MAX_AHEAD_MS, NOW)).toBeNull();
    expect(validateSchedule(NOW + SCHEDULE_MAX_AHEAD_MS + 1, NOW)).toBe('too_late');
    expect(validateSchedule(null, NOW)).toBe('invalid');
    expect(validateSchedule(Number.NaN, NOW)).toBe('invalid');
  });

  it('round-trips <input> values and rejects impossible dates', () => {
    expect(toLocalInputs(Date.UTC(2026, 9, 8, 0, 5), SH)).toEqual({ date: '2026-10-08', time: '08:05' });
    expect(parseLocalInputs('2026-10-08', '08:05', SH)).toBe(Date.UTC(2026, 9, 8, 0, 5));
    expect(parseLocalInputs('2026-02-30', '08:00', SH)).toBeNull();
    expect(parseLocalInputs('2026-10-08', '25:00', SH)).toBeNull();
    expect(parseLocalInputs('', '08:00', SH)).toBeNull();
  });

  it('formats the scheduled time (year only when different)', () => {
    expect(formatScheduleTime(Date.UTC(2026, 9, 8, 0, 0), SH, NOW)).toBe('10月8日周四 08:00');
    expect(formatScheduleTime(Date.UTC(2027, 0, 4, 0, 0), SH, NOW)).toBe('2027年1月4日周一 08:00');
  });
});

describe('SchedulePicker', () => {
  it('schedules a preset', () => {
    const onSchedule = vi.fn();
    const onOpenChange = vi.fn();
    render(<SchedulePicker open onOpenChange={onOpenChange} timeZone={SH} onSchedule={onSchedule} now={() => NOW} />);
    expect(screen.getByText('时区：Asia/Shanghai（GMT+08:00）')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /明天下午 1:00/ }));
    expect(onSchedule).toHaveBeenCalledWith(Date.UTC(2026, 9, 8, 5, 0));
    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  it('validates the custom date and time', () => {
    const onSchedule = vi.fn();
    render(<SchedulePicker open onOpenChange={() => {}} timeZone={SH} onSchedule={onSchedule} now={() => NOW} />);
    fireEvent.click(screen.getByRole('button', { name: '选择日期和时间' }));
    const date = screen.getByLabelText('日期');
    const time = screen.getByLabelText('时间');

    fireEvent.change(date, { target: { value: '2026-10-07' } });
    fireEvent.change(time, { target: { value: '20:03' } });
    fireEvent.click(screen.getByRole('button', { name: '定时发送' }));
    expect(screen.getByRole('alert')).toHaveTextContent('定时时间必须至少在 1 分钟之后');

    fireEvent.change(date, { target: { value: '2026-12-01' } });
    fireEvent.click(screen.getByRole('button', { name: '定时发送' }));
    expect(screen.getByRole('alert')).toHaveTextContent('定时时间不能晚于 30 天之后');
    expect(onSchedule).not.toHaveBeenCalled();

    fireEvent.change(date, { target: { value: '2026-10-09' } });
    fireEvent.change(time, { target: { value: '09:30' } });
    fireEvent.click(screen.getByRole('button', { name: '定时发送' }));
    expect(onSchedule).toHaveBeenCalledWith(Date.UTC(2026, 9, 9, 1, 30));
  });

  it('renders nothing when closed', () => {
    const { container } = render(<SchedulePicker open={false} onOpenChange={() => {}} timeZone={SH} onSchedule={() => {}} />);
    expect(container).toBeEmptyDOMElement();
  });
});
