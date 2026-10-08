/**
 * Regression tests for review findings in the compose window: forwarded attachments (F1), a
 * form remounted while its window stays open (F3), the client-owned signature (F4), IME keys
 * (F8), mailto: links (F15) and saving open drafts before a voluntary logout (F9).
 */
import { act, fireEvent, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import * as endpoints from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Attachment, Draft, DraftInput, DraftSendRequest, DraftUpdateInput, SendResult } from '@/api/types';
import { openMailto, openNewMessage } from '@/components/mail/compose';
import { parseMailto } from '@/lib/emailFrame';
import { useComposeStore } from '@/stores/compose';
import { toast } from '@/stores/toast';
import { ComposeDock, preloadComposeWindow } from './ComposeDock';
import { effectiveInit } from './ComposeWindow';
import { flushAllDrafts } from './useAutosave';
import { seedFromInit, withUnsaved } from './seed';
import {
  installEditorDomPolyfills,
  makeAttachment,
  makeDraft,
  makeMe,
  makeMessage,
  makeQueryClient,
  renderWithProviders,
} from './testFixtures';

installEditorDomPolyfills();

vi.mock('@/api/endpoints', async (importOriginal) => {
  const orig = await importOriginal<typeof import('@/api/endpoints')>();
  return {
    ...orig,
    createDraft: vi.fn(),
    updateDraft: vi.fn(),
    sendDraft: vi.fn(),
    deleteDraft: vi.fn(),
    getDraft: vi.fn(),
    getThread: vi.fn(),
    getMessage: vi.fn(),
    searchContacts: vi.fn(async () => ({ items: [] })),
    uploadAttachment: vi.fn(),
  };
});

const api = vi.mocked(endpoints);
const DEBOUNCE = 30;

let version = 0;
let serverAttachments: Attachment[] = [];
function serverDraft(input: DraftInput, id = 900): Draft {
  return makeDraft({
    id,
    version: ++version,
    mode: input.mode ?? 'new',
    subject: input.subject ?? '',
    to: input.to ?? [],
    html: input.html ?? '',
    attachments: serverAttachments,
  });
}

function sendResult(p: Partial<SendResult> = {}): SendResult {
  return { message_id: 900, thread_id: 10, outbound_id: 3, status: 'queued', undo_ms: 5000, scheduled_at: null, ...p };
}

function renderDock(qc = makeQueryClient()) {
  return renderWithProviders(<ComposeDock testOptions={{ debounceMs: DEBOUNCE }} />, { qc });
}

const sleep = (ms: number) => act(() => new Promise((r) => setTimeout(r, ms)));
const subjectInput = () => screen.getByRole('textbox', { name: '主题' });
const toInput = () => screen.getByRole('combobox', { name: '收件人' });
const windows = () => screen.queryAllByTestId('compose-window');
const signatures = () => document.querySelectorAll('.ProseMirror [data-azm-signature]');
const lastSend = () => api.sendDraft.mock.calls.at(-1)![1] as DraftSendRequest;

function typeTo(text: string) {
  fireEvent.change(toInput(), { target: { value: text } });
  fireEvent.blur(toInput());
}

function sendNow() {
  fireEvent.keyDown(subjectInput(), { key: 'Enter', ctrlKey: true });
}

// The window module is a lazy chunk (F13): load it once so every window renders its form directly.
beforeAll(async () => {
  await preloadComposeWindow();
});

beforeEach(() => {
  version = 0;
  serverAttachments = [];
  useComposeStore.setState({ windows: [], focusOrder: [], focusedKey: null, ownerId: null });
  api.createDraft.mockImplementation(async (input) => serverDraft(input));
  api.updateDraft.mockImplementation(async (id, input: DraftUpdateInput) => serverDraft(input, id));
  api.sendDraft.mockImplementation(async () => sendResult());
  api.deleteDraft.mockResolvedValue(undefined);
});

afterEach(() => {
  act(() => toast.clear());
  vi.clearAllMocks();
});

// ───────────── F1: forwarded attachments ─────────────

describe('forwarding keeps the original attachments (F1)', () => {
  const pdf = makeAttachment({ id: 50, filename: '合同.pdf', size: 2000 });
  const logo = makeAttachment({ id: 51, filename: 'logo.png', content_type: 'image/png', inline: true, content_id: 'logo@x' });
  const copy = makeAttachment({ id: 71, filename: '合同.pdf', size: 2000, download_url: '/api/files/71?d=a', view_url: '/api/files/71?d=i' });
  const logoCopy = makeAttachment({ id: 72, filename: 'logo.png', content_type: 'image/png', inline: true, content_id: 'logo@x' });

  function openForward() {
    const qc = makeQueryClient();
    qc.setQueryData(queryKeys.thread(10), {
      id: 10,
      subject: '周报',
      label_ids: [],
      messages: [makeMessage({ id: 100, attachments: [pdf, logo] })],
    });
    // POST /api/drafts with include_parent_attachments copies them onto the draft.
    api.createDraft.mockImplementation(async (input) => {
      serverAttachments = input.include_parent_attachments ? [copy, logoCopy] : [];
      return serverDraft(input);
    });
    renderDock(qc);
    act(() => void useComposeStore.getState().open({ kind: 'forward', parentMessageId: 100, threadId: 10 }));
  }

  it('shows them right away, adopts the server copies and lists them on every later save and on the send', async () => {
    openForward();
    await screen.findByRole('textbox', { name: '主题' });
    // Shown before any draft exists (only the regular attachment; inline images stay in the quote).
    expect(screen.getByRole('link', { name: '合同.pdf' })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'logo.png' })).not.toBeInTheDocument();

    typeTo('boss@ext.test');
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    expect(api.createDraft.mock.calls[0]![0]).toMatchObject({ mode: 'forward', include_parent_attachments: true });
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());
    // One chip: the server's copy replaced the parent's attachment.
    expect(screen.getAllByRole('link', { name: '合同.pdf' })).toHaveLength(1);
    expect(screen.getByRole('link', { name: '合同.pdf' })).toHaveAttribute('href', expect.stringContaining('/api/files/71'));

    fireEvent.change(subjectInput(), { target: { value: 'Fwd: 周报（请审阅）' } });
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(1));
    expect(api.updateDraft.mock.calls[0]![1].attachment_ids).toEqual([71]);

    sendNow();
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    expect(lastSend().draft!.attachment_ids).toEqual([71]);
  });

  it('a send right after the first save (the draft is created by the send) still carries the copies', async () => {
    openForward();
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(toInput(), { target: { value: 'boss@ext.test' } });
    sendNow(); // commits the recipient, creates the draft (prepareSend), then sends
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    expect(api.createDraft).toHaveBeenCalledTimes(1);
    expect(lastSend().draft!.attachment_ids).toEqual([71]);
    expect(api.updateDraft).not.toHaveBeenCalled(); // no autosave PUT racing the send
  });

  it('an original attachment removed before the first save is dropped from the draft', async () => {
    openForward();
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '移除附件 合同.pdf' }));
    expect(screen.queryByRole('link', { name: '合同.pdf' })).not.toBeInTheDocument();
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    // The server copied it anyway: the next save omits it, so the server deletes the copy.
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(1));
    expect(api.updateDraft.mock.calls[0]![1].attachment_ids).toEqual([]);
    expect(screen.queryByRole('link', { name: '合同.pdf' })).not.toBeInTheDocument();
  });
});

// ───────────── F3: remount after re-login ─────────────

describe('a form remounted while its window stays open (F3)', () => {
  it('reloads its saved draft instead of starting a blank message and a second draft', async () => {
    const first = renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: '周报' } });
    await waitFor(() => expect(useComposeStore.getState().windows[0]!.draftId).toBe(900));
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());
    expect(useComposeStore.getState().windows[0]!.init).toEqual({ kind: 'new' });

    // /login round-trip: the shell (and the dock) unmount; queryClient.clear() drops ['draft'].
    first.unmount();
    api.getDraft.mockResolvedValue(makeDraft({ id: 900, version: 1, subject: '周报', html: '<p>已写的正文</p>' }));
    renderDock(makeQueryClient());
    await waitFor(() => expect(subjectInput()).toHaveValue('周报'));
    expect(api.getDraft).toHaveBeenCalledWith(900, expect.anything());
    expect(document.querySelector('.ProseMirror')).toHaveTextContent('已写的正文');

    fireEvent.change(subjectInput(), { target: { value: '周报（终稿）' } });
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalled());
    expect(api.updateDraft.mock.calls.at(-1)![0]).toBe(900);
    expect(api.updateDraft.mock.calls.at(-1)![1]).toMatchObject({ version: 1, subject: '周报（终稿）' });
    expect(api.createDraft).toHaveBeenCalledTimes(1);
  });

  it('keeps the edits that had not been saved yet and saves them after the remount', async () => {
    const first = renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: '初稿' } });
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());

    // Edited inside the debounce, then the session expires before the save runs.
    fireEvent.change(subjectInput(), { target: { value: '最新的主题' } });
    first.unmount();
    expect(useComposeStore.getState().windows[0]!.unsaved).toMatchObject({ subject: '最新的主题' });
    await sleep(DEBOUNCE * 3);
    expect(api.updateDraft).not.toHaveBeenCalled();

    api.getDraft.mockResolvedValue(makeDraft({ id: 900, version: 1, subject: '初稿' }));
    renderDock(makeQueryClient());
    await waitFor(() => expect(subjectInput()).toHaveValue('最新的主题'));
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(1));
    expect(api.updateDraft.mock.calls[0]![1]).toMatchObject({ version: 1, subject: '最新的主题' });
    expect(useComposeStore.getState().windows[0]!.unsaved).toBeUndefined();
  });

  it('a window without a draft yet keeps its unsaved text too', async () => {
    const first = renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: '还没保存' } });
    first.unmount();
    renderDock(makeQueryClient());
    await waitFor(() => expect(subjectInput()).toHaveValue('还没保存'));
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    expect(api.createDraft.mock.calls[0]![0]).toMatchObject({ mode: 'new', subject: '还没保存' });
  });

  it('effectiveInit / withUnsaved', () => {
    expect(effectiveInit({ draftId: null, init: { kind: 'new' } })).toEqual({ kind: 'new' });
    expect(effectiveInit({ draftId: 5, init: { kind: 'reply', parentMessageId: 1, threadId: 2 } })).toEqual({ kind: 'draft', draftId: 5 });
    const seed = seedFromInit({ kind: 'new' }, makeMe(), null);
    expect(withUnsaved(seed, undefined)).toBe(seed);
    const restored = withUnsaved(seed, {
      fromAddressId: 1,
      to: [{ name: '', email: 'a@b.test' }],
      cc: [],
      bcc: [],
      subject: 's',
      html: '<p>x</p>',
      quotedHtml: null,
      attachments: [],
      editorInlineIds: [],
    });
    expect(restored).toMatchObject({ subject: 's', html: '<p>x</p>', dirty: true, draftId: null });
  });
});

// ───────────── F4: the signature belongs to the client ─────────────

describe('signature (F4)', () => {
  const sigMe = (enabled = true) =>
    makeMe({ settings: { ...makeMe().settings, signature_html: '<p>— 张三</p><p>市场部</p>', signature_enabled: enabled } });

  /** 签名 ▸ item. Waits first: the editor takes focus a frame later (autofocus, `focus()` of the
   *  previous command), which would dismiss a menu opened in the same frame. */
  async function pickSignature(item: RegExp) {
    await sleep(50);
    fireEvent.keyDown(screen.getByRole('button', { name: '插入签名' }), { key: 'Enter' });
    fireEvent.click(within(await screen.findByRole('menu')).getByRole('menuitem', { name: item }));
  }

  it.each(['new', 'reply', 'reply_all', 'forward'] as const)('a %s body starts with exactly one signature, above the quote', async (kind) => {
    const qc = makeQueryClient(sigMe());
    qc.setQueryData(queryKeys.thread(10), { id: 10, subject: '周报', label_ids: [], messages: [makeMessage({ id: 100 })] });
    renderDock(qc);
    act(() =>
      void useComposeStore.getState().open(kind === 'new' ? { kind } : { kind, parentMessageId: 100, threadId: 10 }),
    );
    await screen.findByRole('textbox', { name: '主题' });
    expect(signatures()).toHaveLength(1);
    expect(signatures()[0]).toHaveTextContent('— 张三市场部');
    // The editor ends with the signature; the quote (reply / forward) lives below the editor.
    expect(document.querySelector('.ProseMirror')!.lastElementChild).toHaveAttribute('data-azm-signature');
  });

  it('不使用签名 removes it from what is sent; 插入签名 puts back exactly one copy', async () => {
    renderDock(makeQueryClient(sigMe()));
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.test' }], subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    expect(signatures()).toHaveLength(1);

    await pickSignature(/不使用签名/);
    expect(signatures()).toHaveLength(0);

    await pickSignature(/插入签名/);
    expect(signatures()).toHaveLength(1);
    await pickSignature(/插入签名/);
    expect(signatures()).toHaveLength(1); // replaced, never duplicated

    await pickSignature(/不使用签名/);
    expect(signatures()).toHaveLength(0);
    sendNow();
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    const html = lastSend().draft!.html!;
    expect(html).not.toContain('— 张三');
    expect(html).not.toContain('data-azm-signature');
  });

  it('sends an edited signature as edited (one copy) and an image-only signature once', async () => {
    const me = makeMe({
      settings: { ...makeMe().settings, signature_html: '<p><img src="https://cdn.test/logo.png" alt="logo"></p>', signature_enabled: true },
    });
    renderDock(makeQueryClient(me));
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.test' }], subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    sendNow();
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    const html = lastSend().draft!.html!;
    expect(html.match(/cdn\.test\/logo\.png/g)).toHaveLength(1);
  });

  it('signature_enabled=false starts without one; 插入签名 still inserts it', async () => {
    renderDock(makeQueryClient(sigMe(false)));
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    expect(signatures()).toHaveLength(0);
    await pickSignature(/插入签名/);
    expect(signatures()).toHaveLength(1);
  });
});

// ───────────── F8: IME keys ─────────────

describe('IME composition keys (F8)', () => {
  it('Esc / Enter that belong to an input method neither close nor send the window', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.test' }], subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    const body = document.querySelector('.ProseMirror')!;
    fireEvent.keyDown(body, { key: 'Escape', isComposing: true });
    fireEvent.keyDown(subjectInput(), { key: 'Escape', keyCode: 229 });
    fireEvent.keyDown(subjectInput(), { key: 'Enter', ctrlKey: true, isComposing: true });
    await sleep(DEBOUNCE * 3);
    expect(windows()).toHaveLength(1);
    expect(api.sendDraft).not.toHaveBeenCalled();
    // A real Esc still closes it.
    fireEvent.keyDown(subjectInput(), { key: 'Escape' });
    await waitFor(() => expect(windows()).toHaveLength(0));
  });
});

// ───────────── F15: mailto: links ─────────────

describe('mailto: links (F15)', () => {
  it('open a message with To, Cc, Bcc, subject and body (Cc is not merged into To)', async () => {
    renderDock(makeQueryClient(makeMe({ settings: { ...makeMe().settings, signature_html: '<p>— 张三</p>' } })));
    act(() => void openMailto(parseMailto('mailto:a@x.test?cc=boss@x.test&bcc=audit@x.test&subject=%E4%BD%A0%E5%A5%BD&body=%E7%AC%AC%E4%B8%80%E8%A1%8C%0A%E7%AC%AC%E4%BA%8C%E8%A1%8C')!));
    expect(useComposeStore.getState().windows[0]!.init).toEqual({
      kind: 'new',
      to: [{ name: '', email: 'a@x.test' }],
      cc: [{ name: '', email: 'boss@x.test' }],
      bcc: [{ name: '', email: 'audit@x.test' }],
      subject: '你好',
      body: '第一行\n第二行',
    });
    await screen.findByRole('textbox', { name: '主题' });
    expect(subjectInput()).toHaveValue('你好');
    expect(screen.getByRole('combobox', { name: '收件人' }).closest('.cw-field')).toHaveTextContent('a@x.test');
    expect(screen.getByRole('combobox', { name: '抄送' }).closest('.cw-field')).toHaveTextContent('boss@x.test');
    expect(screen.getByRole('combobox', { name: '密送' }).closest('.cw-field')).toHaveTextContent('audit@x.test');
    expect(screen.getByRole('combobox', { name: '收件人' }).closest('.cw-field')).not.toHaveTextContent('boss@x.test');
    const paragraphs = Array.from(document.querySelectorAll('.ProseMirror > p')).map((p) => p.textContent);
    expect(paragraphs.slice(0, 2)).toEqual(['第一行', '第二行']);
    expect(signatures()).toHaveLength(1); // the text comes before the signature

    sendNow();
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    expect(lastSend().draft).toMatchObject({
      to: [{ name: '', email: 'a@x.test' }],
      cc: [{ name: '', email: 'boss@x.test' }],
      bcc: [{ name: '', email: 'audit@x.test' }],
    });
    expect(lastSend().draft!.html).toContain('第一行');
  });

  it('openNewMessage leaves out empty extras', () => {
    act(() => void openNewMessage(undefined, undefined, { cc: [], body: '  ' }));
    expect(useComposeStore.getState().windows[0]!.init).toEqual({ kind: 'new' });
  });
});

// ───────────── F9: 退出登录 saves open drafts first ─────────────

describe('flushAllDrafts (F9)', () => {
  it('saves the edits of every open window that are still inside the autosave debounce', async () => {
    // A long debounce: without the flush nothing would reach the server before the logout.
    renderWithProviders(<ComposeDock testOptions={{ debounceMs: 60_000 }} />, { qc: makeQueryClient() });
    act(() => {
      useComposeStore.getState().open({ kind: 'new' });
      useComposeStore.getState().open({ kind: 'new' });
    });
    await waitFor(() => expect(screen.getAllByRole('textbox', { name: '主题' })).toHaveLength(2));
    const [first, second] = screen.getAllByRole('textbox', { name: '主题' });
    fireEvent.change(first!, { target: { value: '周报' } });
    fireEvent.change(second!, { target: { value: '月报' } });
    await sleep(10);
    expect(api.createDraft).not.toHaveBeenCalled();

    await act(() => flushAllDrafts(2000));
    expect(api.createDraft.mock.calls.map((c) => c[0].subject).sort()).toEqual(['周报', '月报']);
  });

  it('resolves after the timeout when a save hangs, and right away with nothing to save', async () => {
    renderWithProviders(<ComposeDock testOptions={{ debounceMs: 60_000 }} />, { qc: makeQueryClient() });
    await act(() => flushAllDrafts(50)); // no windows
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    api.createDraft.mockImplementation(() => new Promise<Draft>(() => {}));
    fireEvent.change(subjectInput(), { target: { value: 'x' } });
    const started = Date.now();
    await act(() => flushAllDrafts(80));
    expect(Date.now() - started).toBeLessThan(2000);
    expect(api.createDraft).toHaveBeenCalledTimes(1);
  });
});
