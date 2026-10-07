import { QueryClientProvider } from '@tanstack/react-query';
import { act, renderHook, screen } from '@testing-library/react';
import type { ReactNode } from 'react';
import { MemoryRouter, Route, Routes } from 'react-router';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '@/api/client';
import * as endpoints from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import { useComposeStore } from '@/stores/compose';
import { toast, useToastStore } from '@/stores/toast';
import { makeDraft, makeQueryClient } from './testFixtures';
import { DraftAutosaver } from './useAutosave';
import { preSendCheck, recipientFieldOf, undoSentMessage, useDraftSend } from './useDraftSend';

vi.mock('@/api/endpoints', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/api/endpoints')>()),
  sendDraft: vi.fn(),
  undoSend: vi.fn(),
}));
const api = vi.mocked(endpoints);
const A = (email: string) => ({ name: '', email });

afterEach(() => {
  act(() => toast.clear());
  useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null });
});

describe('preSendCheck', () => {
  it('requires at least one valid recipient and ≤ 50 per field', () => {
    expect(preSendCheck({ to: [], cc: [], bcc: [] })).toEqual({ kind: 'no_recipients' });
    expect(preSendCheck({ to: [], cc: [], bcc: [A('a@b.com')] })).toBeNull();
    expect(preSendCheck({ to: [A('a@b.com')], cc: [A('bad')], bcc: [] })).toEqual({ kind: 'invalid_address', address: 'bad', field: 'cc' });
    const many = Array.from({ length: 51 }, (_, i) => A(`u${i}@b.com`));
    expect(preSendCheck({ to: many.slice(0, 50), cc: [], bcc: many })).toEqual({ kind: 'too_many', field: 'bcc' });
  });

  it('maps the server field of too_many_recipients', () => {
    expect(recipientFieldOf('draft.cc')).toBe('cc');
    expect(recipientFieldOf('to')).toBe('to');
    expect(recipientFieldOf('subject')).toBeNull();
    expect(recipientFieldOf(undefined)).toBeNull();
  });
});

function setup() {
  const qc = makeQueryClient();
  const saver = new DraftAutosaver({ draftId: 5, version: 2, collect: () => ({}), createFields: () => ({}) });
  const key = useComposeStore.getState().open({ kind: 'draft', draftId: 5 });
  const wrapper = ({ children }: { children: ReactNode }) => (
    <QueryClientProvider client={qc}>
      <MemoryRouter initialEntries={['/mail/inbox']}>
        {children}
        <Routes>
          <Route path="/mail/scheduled/:id" element={<p>scheduled-view</p>} />
          <Route path="*" element={null} />
        </Routes>
      </MemoryRouter>
    </QueryClientProvider>
  );
  const { result } = renderHook(
    () => useDraftSend({ winKey: key, saver, collect: () => ({ subject: 's' }), timeZone: 'Asia/Shanghai' }),
    { wrapper },
  );
  return { qc, key, send: () => result.current };
}

describe('useDraftSend', () => {
  it('scheduled send: closes the window and toasts the time with 查看邮件 → /mail/scheduled', async () => {
    const at = Date.UTC(2026, 9, 8, 0, 0);
    api.sendDraft.mockResolvedValue({ message_id: 5, thread_id: 10, outbound_id: 1, status: 'queued', undo_ms: 0, scheduled_at: at });
    const { qc, send } = setup();
    qc.setQueryData(queryKeys.draft(5), makeDraft({ id: 5 }));
    let outcome: Awaited<ReturnType<ReturnType<typeof useDraftSend>>> | undefined;
    await act(async () => {
      outcome = await send()(at);
    });
    expect(outcome?.ok).toBe(true);
    expect(api.sendDraft).toHaveBeenCalledWith(5, { version: 2, draft: { subject: 's' }, scheduled_at: at });
    expect(useComposeStore.getState().windows).toHaveLength(0);
    expect(qc.getQueryData(queryKeys.draft(5))).toBeUndefined();
    const t = useToastStore.getState().toasts[0]!;
    expect(t.message).toMatch(/^邮件已定时，将于 (2026年)?10月8日周四 08:00 发送$/);
    expect(t.actions.map((a) => a.label)).toEqual(['查看邮件']);
    act(() => t.actions[0]!.onClick());
    expect(await screen.findByText('scheduled-view')).toBeInTheDocument();
  });

  it('returns errors to the window and dismisses the 正在发送… toast', async () => {
    api.sendDraft.mockRejectedValue(new ApiError(403, 'send_as_forbidden', '无权限'));
    const { send } = setup();
    let outcome: Awaited<ReturnType<ReturnType<typeof useDraftSend>>> | undefined;
    await act(async () => {
      outcome = await send()(null);
    });
    expect(outcome).toMatchObject({ ok: false });
    expect(useToastStore.getState().toasts).toHaveLength(0);
    expect(useComposeStore.getState().windows).toHaveLength(1);
  });
});

describe('undoSentMessage', () => {
  it('reports too_late', async () => {
    api.undoSend.mockRejectedValue(new ApiError(409, 'too_late', 'x'));
    const open = vi.fn();
    await act(async () => {
      expect(await undoSentMessage({ message_id: 1, thread_id: 2 }, { qc: makeQueryClient(), open })).toBe(false);
    });
    expect(open).not.toHaveBeenCalled();
    expect(useToastStore.getState().toasts[0]).toMatchObject({ message: '已无法撤销，邮件已发出', tone: 'error' });
  });

  it('seeds the draft cache before reopening', async () => {
    const draft = makeDraft({ id: 8, version: 4 });
    api.undoSend.mockResolvedValue({ draft });
    const qc = makeQueryClient();
    const open = vi.fn((id: number) => expect(qc.getQueryData(queryKeys.draft(id))).toEqual(draft));
    await act(async () => {
      expect(await undoSentMessage({ message_id: 8, thread_id: 2 }, { qc, open })).toBe(true);
    });
    expect(open).toHaveBeenCalledWith(8);
  });
});
