import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DEFAULT_TOAST_MS, MAX_TOASTS, toast, useToastStore } from './toast';

describe('toast store', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    toast.clear();
    vi.useRealTimers();
  });

  it('push returns an id and auto-dismisses after durationMs', () => {
    const id = toast.push({ message: '会话已归档' });
    expect(useToastStore.getState().toasts.map((t) => t.id)).toEqual([id]);
    vi.advanceTimersByTime(DEFAULT_TOAST_MS - 1);
    expect(useToastStore.getState().toasts).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(useToastStore.getState().toasts).toHaveLength(0);
  });

  it('keeps sticky toasts and supports manual dismiss', () => {
    const id = toast.push({ message: '正在发送…', durationMs: Infinity });
    vi.advanceTimersByTime(60_000);
    expect(useToastStore.getState().toasts).toHaveLength(1);
    toast.dismiss(id);
    expect(useToastStore.getState().toasts).toHaveLength(0);
  });

  it('treats 0 / negative durations (undo_ms = 0) as the default, not sticky', () => {
    toast.push({ message: '邮件已发送', durationMs: 0 });
    toast.push({ message: 'x', durationMs: -5 });
    vi.advanceTimersByTime(DEFAULT_TOAST_MS);
    expect(useToastStore.getState().toasts).toHaveLength(0);
  });

  it('replaces a toast pushed with the same id and restarts its timer', () => {
    toast.push({ id: 'send', message: '正在发送…', durationMs: Infinity });
    vi.advanceTimersByTime(1000);
    toast.push({ id: 'send', message: '邮件已发送', durationMs: 2000, action: { label: '撤销', onClick: () => {} } });
    const [t] = useToastStore.getState().toasts;
    expect(useToastStore.getState().toasts).toHaveLength(1);
    expect(t).toMatchObject({ id: 'send', message: '邮件已发送' });
    expect(t!.actions.map((a) => a.label)).toEqual(['撤销']);
    vi.advanceTimersByTime(2000);
    expect(useToastStore.getState().toasts).toHaveLength(0);
  });

  it('merges action + actions and caps the stack', () => {
    const onUndo = vi.fn();
    toast.push({
      message: '邮件已发送',
      action: { label: '撤销', onClick: onUndo },
      actions: [{ label: '查看邮件', onClick: () => {} }],
    });
    expect(useToastStore.getState().toasts[0]!.actions.map((a) => a.label)).toEqual(['撤销', '查看邮件']);
    for (let i = 0; i < MAX_TOASTS + 2; i++) toast.push({ message: `m${i}` });
    const msgs = useToastStore.getState().toasts.map((t) => t.message);
    expect(msgs).toHaveLength(MAX_TOASTS);
    expect(msgs.at(-1)).toBe(`m${MAX_TOASTS + 1}`);
  });
});
