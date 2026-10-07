/**
 * Global toast (snackbar) store, shared by every package. Rendered by `<ToastHost />`
 * bottom-left like Gmail.
 *
 *   const view = { label: '查看邮件', onClick: openMessage };
 *   const id = toast.push({
 *     message: '邮件已发送',
 *     // No 撤销 when undo_ms === 0 (undo_send_seconds = 0, or a scheduled send): undo would 409 too_late.
 *     actions: res.undo_ms > 0 ? [{ label: '撤销', onClick: undo }, view] : [view],
 *     durationMs: res.undo_ms > 0 ? res.undo_ms : undefined,
 *   });
 *   toast.dismiss(id);
 *
 * - `durationMs` defaults to DEFAULT_TOAST_MS (5 s). Only `Infinity` keeps the toast until
 *   dismissed (e.g. "正在发送…"); `0`, negative or NaN values fall back to the default.
 * - Clicking an action runs `onClick` and dismisses the toast (unless `keepOpen`).
 * - Pushing with an existing `id` replaces that toast in place (e.g. "正在发送…" → "邮件已发送").
 * - At most MAX_TOASTS are kept; the oldest are dropped.
 */
import { create } from 'zustand';

export interface ToastAction {
  label: string;
  onClick: () => void;
  /** Keep the toast open after the click (default: dismiss). */
  keepOpen?: boolean;
}

export type ToastTone = 'default' | 'error';

export interface ToastInput {
  message: string;
  /** Single action (contract shape). */
  action?: ToastAction;
  /** Several actions, e.g. 撤销 · 查看邮件. Rendered after `action`. */
  actions?: ToastAction[];
  durationMs?: number;
  /** Reuse an id to replace an existing toast. */
  id?: string;
  tone?: ToastTone;
}

export interface Toast {
  id: string;
  message: string;
  actions: ToastAction[];
  durationMs: number;
  tone: ToastTone;
  createdAt: number;
}

export interface ToastState {
  toasts: Toast[];
  push(t: ToastInput): string;
  dismiss(id: string): void;
  clear(): void;
}

export const DEFAULT_TOAST_MS = 5000;
export const MAX_TOASTS = 3;

/** `Infinity` = sticky; anything else that is not a positive finite number → the default. */
function normalizeDuration(ms: number | undefined): number {
  if (ms === Number.POSITIVE_INFINITY) return ms;
  return ms !== undefined && Number.isFinite(ms) && ms > 0 ? ms : DEFAULT_TOAST_MS;
}

let seq = 0;
const timers = new Map<string, ReturnType<typeof setTimeout>>();

function clearTimer(id: string) {
  const h = timers.get(id);
  if (h !== undefined) {
    clearTimeout(h);
    timers.delete(id);
  }
}

export const useToastStore = create<ToastState>()((set, get) => ({
  toasts: [],

  push(input) {
    const id = input.id ?? `t${++seq}`;
    const durationMs = normalizeDuration(input.durationMs);
    const next: Toast = {
      id,
      message: input.message,
      actions: [...(input.action ? [input.action] : []), ...(input.actions ?? [])],
      durationMs,
      tone: input.tone ?? 'default',
      createdAt: Date.now(),
    };
    clearTimer(id);
    const existing = get().toasts;
    const toasts = existing.some((x) => x.id === id)
      ? existing.map((x) => (x.id === id ? next : x))
      : [...existing, next];
    const dropped = toasts.slice(0, Math.max(0, toasts.length - MAX_TOASTS));
    for (const d of dropped) clearTimer(d.id);
    set({ toasts: toasts.slice(-MAX_TOASTS) });
    if (Number.isFinite(durationMs)) {
      timers.set(
        id,
        setTimeout(() => get().dismiss(id), durationMs),
      );
    }
    return id;
  },

  dismiss(id) {
    clearTimer(id);
    if (get().toasts.some((x) => x.id === id)) set({ toasts: get().toasts.filter((x) => x.id !== id) });
  },

  clear() {
    for (const id of timers.keys()) clearTimer(id);
    set({ toasts: [] });
  },
}));

/** Imperative facade usable outside React. */
export const toast = {
  push: (t: ToastInput): string => useToastStore.getState().push(t),
  dismiss: (id: string): void => useToastStore.getState().dismiss(id),
  clear: (): void => useToastStore.getState().clear(),
  /** Shorthand for an error toast. */
  error: (message: string, durationMs = 8000): string =>
    useToastStore.getState().push({ message, tone: 'error', durationMs }),
};
