/**
 * Send flow [WP-F] (DESIGN.md §5 "Send"):
 *   1. client checks (`preSendCheck`: recipients present, valid, ≤ 50 per field; the window asks
 *      about an empty subject);
 *   2. `saver.prepareSend()` — wait for in-flight autosaves, create the draft if needed;
 *   3. POST /api/drafts/:id/send with `{version, draft: <final fields>, scheduled_at}`;
 *   4. close the window, then a global toast: "邮件已发送 · 撤销 · 查看邮件" for `undo_ms`
 *      (撤销 omitted when undo_ms === 0), or "邮件已定时，将于 … 发送 · 查看邮件";
 *   5. 撤销 → POST undo-send → seed `['draft', id]` with the returned draft → reopen it.
 * Errors are returned to the window, which maps them (conflict dialog, red chips, alerts).
 */
import { useQueryClient, type QueryClient } from '@tanstack/react-query';
import { useCallback } from 'react';
import { useNavigate } from 'react-router';
import { errorMessage, isApiError } from '@/api/client';
import { sendDraft, undoSend } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Address, DraftInput, SendResult } from '@/api/types';
import { t } from '@/i18n/zh';
import { isValidEmail, MAX_RECIPIENTS_PER_FIELD } from '@/lib/recipients';
import { useComposeStore } from '@/stores/compose';
import { toast } from '@/stores/toast';
import { formatScheduleTime } from './schedule';
import type { DraftAutosaver } from './useAutosave';

export type RecipientFieldName = 'to' | 'cc' | 'bcc';

export type PreSendProblem =
  | { kind: 'no_recipients' }
  | { kind: 'invalid_address'; address: string; field: RecipientFieldName }
  | { kind: 'too_many'; field: RecipientFieldName };

/** Client-side checks before sending (the server re-validates everything). */
export function preSendCheck(fields: { to: readonly Address[]; cc: readonly Address[]; bcc: readonly Address[] }): PreSendProblem | null {
  const order: RecipientFieldName[] = ['to', 'cc', 'bcc'];
  for (const f of order) {
    const bad = fields[f].find((a) => !isValidEmail(a.email));
    if (bad) return { kind: 'invalid_address', address: bad.email, field: f };
  }
  for (const f of order) if (fields[f].length > MAX_RECIPIENTS_PER_FIELD) return { kind: 'too_many', field: f };
  if (fields.to.length + fields.cc.length + fields.bcc.length === 0) return { kind: 'no_recipients' };
  return null;
}

export const fieldLabel = (f: RecipientFieldName): string => t(`compose.${f}` as const);

/** Normalizes the server's `details.field` ("to", "draft.cc", …) to a recipient field. */
export function recipientFieldOf(field: unknown): RecipientFieldName | null {
  if (typeof field !== 'string') return null;
  const last = field.split('.').pop();
  return last === 'to' || last === 'cc' || last === 'bcc' ? last : null;
}

function invalidateMail(qc: QueryClient, threadId?: number) {
  void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
  void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  if (threadId !== undefined) void qc.invalidateQueries({ queryKey: queryKeys.thread(threadId) });
}

export interface UndoDeps {
  qc: QueryClient;
  /** Opens the restored draft (compose store). */
  open?: (draftId: number) => void;
}

/** 撤销: cancel the queued send and reopen the draft in a compose window. */
export async function undoSentMessage(result: Pick<SendResult, 'message_id' | 'thread_id'>, deps: UndoDeps): Promise<boolean> {
  const id = toast.push({ message: t('compose.toasts.undoing'), durationMs: Number.POSITIVE_INFINITY });
  try {
    const { draft } = await undoSend(result.message_id);
    // ['draft', id] is never refetched (staleTime Infinity): seed it, or the reopened window's
    // first autosave would carry an old version and fail with version_conflict.
    deps.qc.setQueryData(queryKeys.draft(draft.id), draft);
    (deps.open ?? ((draftId) => useComposeStore.getState().open({ kind: 'draft', draftId })))(draft.id);
    invalidateMail(deps.qc, result.thread_id);
    toast.push({ id, message: t('compose.toasts.undone') });
    return true;
  } catch (e) {
    toast.push({ id, message: isApiError(e, 'too_late') ? t('errors.too_late') : errorMessage(e), tone: 'error', durationMs: 8000 });
    return false;
  }
}

export type SendOutcome = { ok: true; result: SendResult } | { ok: false; error: unknown };

export interface UseDraftSendOptions {
  winKey: string;
  saver: DraftAutosaver;
  /** Final field values (DraftInput). */
  collect: () => DraftInput;
  /** Display timezone for the scheduled toast. */
  timeZone: string;
}

export function useDraftSend({ winKey, saver, collect, timeZone }: UseDraftSendOptions) {
  const qc = useQueryClient();
  const navigate = useNavigate();

  return useCallback(
    async (scheduledAt: number | null = null): Promise<SendOutcome> => {
      const toastId = toast.push({ message: t('compose.toasts.sending'), durationMs: Number.POSITIVE_INFINITY });
      let result: SendResult;
      try {
        const { draftId, version } = await saver.prepareSend();
        result = await sendDraft(draftId, { version, draft: collect(), scheduled_at: scheduledAt });
      } catch (error) {
        toast.dismiss(toastId);
        return { ok: false, error };
      }

      saver.dispose();
      const store = useComposeStore.getState();
      const draftId = saver.draftId;
      store.close(winKey);
      if (draftId !== null) qc.removeQueries({ queryKey: queryKeys.draft(draftId), exact: true });
      invalidateMail(qc, result.thread_id);

      const folder = result.scheduled_at !== null ? 'scheduled' : 'sent';
      const view = { label: t('compose.toasts.view'), onClick: () => void navigate(`/mail/${folder}/${result.thread_id}`) };
      if (result.scheduled_at !== null) {
        toast.push({
          id: toastId,
          message: t('compose.toasts.scheduled', { time: formatScheduleTime(result.scheduled_at, timeZone) }),
          actions: [view],
        });
      } else {
        const canUndo = result.undo_ms > 0;
        toast.push({
          id: toastId,
          message: t('compose.toasts.sent'),
          actions: canUndo ? [{ label: t('compose.toasts.undo'), onClick: () => void undoSentMessage(result, { qc }) }, view] : [view],
          durationMs: canUndo ? result.undo_ms : undefined,
        });
      }
      return { ok: true, result };
    },
    [qc, navigate, saver, collect, winKey, timeZone],
  );
}
