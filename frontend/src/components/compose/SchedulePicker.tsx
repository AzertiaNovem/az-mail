/**
 * "定时发送" dialog [WP-F]: Gmail's presets (明天上午 8:00 / 明天下午 1:00 / 下周一上午 8:00)
 * and a custom date + time picker, in the user's display timezone (shown), validated to
 * 1 minute – 30 days ahead. Also used to reschedule a scheduled message (`title`,
 * `confirmLabel`, `currentAt`: the custom picker starts at the current time, which is shown).
 */
import { useState, type FormEvent } from 'react';
import { Button, Dialog, FieldMessage, Icon, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { safeTimeZone } from '@/lib/quote';
import {
  formatGmtOffset,
  formatScheduleTime,
  parseLocalInputs,
  schedulePresets,
  toLocalInputs,
  validateSchedule,
  type ScheduleError,
  type SchedulePresetKey,
} from './schedule';

const PRESET_LABELS: Record<SchedulePresetKey, 'compose.schedule.presetTomorrowMorning' | 'compose.schedule.presetTomorrowAfternoon' | 'compose.schedule.presetMondayMorning'> = {
  tomorrowMorning: 'compose.schedule.presetTomorrowMorning',
  tomorrowAfternoon: 'compose.schedule.presetTomorrowAfternoon',
  mondayMorning: 'compose.schedule.presetMondayMorning',
};

const ERROR_LABELS: Record<ScheduleError, 'compose.schedule.invalid' | 'compose.schedule.tooSoon' | 'compose.schedule.tooLate'> = {
  invalid: 'compose.schedule.invalid',
  too_soon: 'compose.schedule.tooSoon',
  too_late: 'compose.schedule.tooLate',
};

export interface SchedulePickerProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  timeZone: string;
  onSchedule: (at: number) => void;
  /** Dialog title (default 定时发送). */
  title?: string;
  /** Custom picker's confirm button (default 定时发送). */
  confirmLabel?: string;
  /** The time already scheduled (reschedule): shown, and the custom picker starts from it. */
  currentAt?: number | null;
  /** Clock (tests). */
  now?: () => number;
}

export function SchedulePicker(props: SchedulePickerProps) {
  // Remount on open so presets and defaults are computed from the current time.
  return props.open ? <SchedulePickerBody {...props} /> : null;
}

function SchedulePickerBody({ onOpenChange, timeZone, onSchedule, title, confirmLabel, currentAt = null, now = Date.now }: SchedulePickerProps) {
  const tz = safeTimeZone(timeZone);
  const [opened] = useState(now);
  const presets = schedulePresets(opened, tz);
  const [custom, setCustom] = useState(false);
  const initial = toLocalInputs(currentAt ?? presets[0]?.at ?? opened + 3_600_000, tz);
  const [date, setDate] = useState(initial.date);
  const [time, setTime] = useState(initial.time);
  const [error, setError] = useState<ScheduleError | null>(null);

  const pick = (at: number) => {
    const err = validateSchedule(at, now());
    if (err) {
      setError(err);
      setCustom(true);
      return;
    }
    onOpenChange(false);
    onSchedule(at);
  };

  const submitCustom = (e?: FormEvent) => {
    e?.preventDefault();
    const at = parseLocalInputs(date, time, tz);
    const err = validateSchedule(at, now());
    if (err || at === null) {
      setError(err ?? 'invalid');
      return;
    }
    onOpenChange(false);
    onSchedule(at);
  };

  const tzLine = (
    <>
      {currentAt !== null && (
        <p className="text-xs text-on-surface-variant">{t('compose.schedule.current', { time: formatScheduleTime(currentAt, tz, opened) })}</p>
      )}
      <p className="text-xs text-on-surface-variant">
        {t('compose.schedule.timezone', { tz, offset: formatGmtOffset(opened, tz) })}
      </p>
    </>
  );

  return (
    <Dialog
      open
      onOpenChange={onOpenChange}
      title={custom ? t('compose.schedule.custom') : (title ?? t('compose.schedule.title'))}
      size="sm"
      footer={
        custom ? (
          <>
            <Button variant="text" onClick={() => setCustom(false)}>
              {t('compose.schedule.back')}
            </Button>
            <Button onClick={() => submitCustom()}>{confirmLabel ?? t('compose.schedule.confirm')}</Button>
          </>
        ) : (
          <Button variant="text" onClick={() => onOpenChange(false)}>
            {t('actions.cancel')}
          </Button>
        )
      }
    >
      {custom ? (
        <form className="flex flex-col gap-4" onSubmit={submitCustom} noValidate>
          <div className="grid grid-cols-[1fr_8rem] gap-3">
            <TextField
              type="date"
              label={t('compose.schedule.date')}
              value={date}
              onChange={(e) => {
                setDate(e.target.value);
                setError(null);
              }}
              aria-invalid={error !== null || undefined}
              required
            />
            <TextField
              type="time"
              label={t('compose.schedule.time')}
              value={time}
              step={60}
              onChange={(e) => {
                setTime(e.target.value);
                setError(null);
              }}
              aria-invalid={error !== null || undefined}
              required
            />
          </div>
          {error && (
            <div role="alert">
              <FieldMessage id="schedule-error" error>
                {t(ERROR_LABELS[error])}
              </FieldMessage>
            </div>
          )}
          {tzLine}
          <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
        </form>
      ) : (
        <div className="-mx-6 flex flex-col">
          {presets.map((p) => (
            <button key={p.key} type="button" className="cw-schedule-option" onClick={() => pick(p.at)}>
              <span className="flex-1 text-left">{t(PRESET_LABELS[p.key])}</span>
              <span className="text-on-surface-variant">{formatScheduleTime(p.at, tz, opened)}</span>
            </button>
          ))}
          <div className="mx-6 my-1 h-px bg-divider" />
          <button type="button" className="cw-schedule-option" onClick={() => setCustom(true)}>
            <Icon name="calendar_month" size={20} className="text-on-surface-variant" />
            <span className="flex-1 text-left">{t('compose.schedule.custom')}</span>
          </button>
          <div className="px-6 pt-3">{tzLine}</div>
        </div>
      )}
    </Dialog>
  );
}
