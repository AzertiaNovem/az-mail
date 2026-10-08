import { act, fireEvent, screen, waitFor, within } from '@testing-library/react';
import type { Editor } from '@tiptap/core';
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '@/api/client';
import * as endpoints from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Draft, DraftInput, DraftSendRequest, DraftUpdateInput, SendResult } from '@/api/types';
import { useComposeStore } from '@/stores/compose';
import { toast, useToastStore } from '@/stores/toast';
import { ComposeDock, preloadComposeWindow } from './ComposeDock';
import {
  fakeFile,
  installEditorDomPolyfills,
  makeAttachment,
  makeDraft,
  makeIdentity,
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
    undoSend: vi.fn(),
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
function serverDraft(input: DraftInput, id = 900): Draft {
  return makeDraft({
    id,
    version: ++version,
    mode: input.mode ?? 'new',
    subject: input.subject ?? '',
    to: input.to ?? [],
    html: input.html ?? '',
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
const editorOf = (el: Element = document.body): Editor =>
  (el.querySelector('.ProseMirror') as unknown as { editor: Editor }).editor;
const toastTexts = () => useToastStore.getState().toasts.map((t) => t.message);

function typeTo(text: string) {
  fireEvent.change(toInput(), { target: { value: text } });
  fireEvent.blur(toInput());
}

// The window module is a lazy chunk (F13): load it once so every window renders its form directly.
beforeAll(async () => {
  await preloadComposeWindow();
});

beforeEach(() => {
  version = 0;
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

describe('ComposeWindow — drafts', () => {
  it('creates the draft lazily on the first edit, then autosaves with the version', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    expect(screen.getByRole('heading', { name: '新邮件' })).toBeInTheDocument();
    await sleep(DEBOUNCE * 4);
    expect(api.createDraft).not.toHaveBeenCalled();

    fireEvent.change(subjectInput(), { target: { value: '周报' } });
    expect(screen.getByRole('heading', { name: '周报' })).toBeInTheDocument();
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    expect(api.createDraft.mock.calls[0]![0]).toMatchObject({ mode: 'new', parent_message_id: null, subject: '周报', to: [], attachment_ids: [] });
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());
    expect(useComposeStore.getState().windows[0]!.draftId).toBe(900);

    act(() => void editorOf().commands.insertContent('正文内容'));
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(1));
    const [id, body] = api.updateDraft.mock.calls[0]!;
    expect(id).toBe(900);
    expect(body).toMatchObject({ version: 1, subject: '周报' });
    expect(body.html).toContain('正文内容');
    expect(api.createDraft).toHaveBeenCalledTimes(1);
  });

  it('opens a reply with recipients, subject, alias From and the quote — without creating a draft', async () => {
    const qc = makeQueryClient(
      makeMe({
        identities: [makeIdentity(), makeIdentity({ address_id: 9, email: 'support@az.test', display_name: '客服', kind: 'alias', is_default: false })],
        settings: { ...makeMe().settings, signature_html: '<p>— 张三</p>' },
      }),
    );
    const parent = makeMessage({ id: 100, delivered_to: 'support@az.test', to: [{ name: '客服', email: 'support@az.test' }], cc: [{ name: '', email: 'cc@ext.test' }] });
    qc.setQueryData(queryKeys.thread(10), { id: 10, subject: '周报', label_ids: [], messages: [parent] });
    renderDock(qc);
    act(() => void useComposeStore.getState().open({ kind: 'reply_all', parentMessageId: 100, threadId: 10 }));
    await screen.findByRole('textbox', { name: '主题' });
    expect(subjectInput()).toHaveValue('Re: 周报');
    expect(screen.getByLabelText('发件人')).toHaveValue('9');
    expect(screen.getByRole('button', { name: '移除 李四' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '移除 cc@ext.test' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: '移除 客服' })).not.toBeInTheDocument();
    expect(document.querySelector('.ProseMirror [data-azm-signature]')).toHaveTextContent('— 张三');

    fireEvent.click(screen.getByRole('button', { name: '显示被剪辑的内容' }));
    expect(screen.getByTitle('引用的原邮件')).toBeInTheDocument();

    await sleep(DEBOUNCE * 5);
    expect(api.createDraft).not.toHaveBeenCalled();

    // The first edit creates the reply draft with the create-only fields and the quote.
    fireEvent.change(subjectInput(), { target: { value: 'Re: 周报（修订）' } });
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    const input = api.createDraft.mock.calls[0]![0];
    expect(input).toMatchObject({ mode: 'reply_all', parent_message_id: 100, from_address_id: 9 });
    expect(input.quoted_html).toContain('写道：');
    expect(input.include_parent_attachments).toBeUndefined();
  });

  it('forward drafts ask the server to copy the parent attachments', async () => {
    const qc = makeQueryClient();
    qc.setQueryData(queryKeys.thread(10), { id: 10, subject: '周报', label_ids: [], messages: [makeMessage({ id: 100 })] });
    renderDock(qc);
    act(() => void useComposeStore.getState().open({ kind: 'forward', parentMessageId: 100, threadId: 10 }));
    await screen.findByRole('textbox', { name: '主题' });
    expect(subjectInput()).toHaveValue('Fwd: 周报');
    typeTo('boss@ext.test');
    await waitFor(() => expect(api.createDraft).toHaveBeenCalledTimes(1));
    expect(api.createDraft.mock.calls[0]![0]).toMatchObject({ mode: 'forward', include_parent_attachments: true });
  });

  it('shows the conflict dialog on 409 and overwrites with force', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: 'a' } });
    await waitFor(() => expect(api.createDraft).toHaveBeenCalled());
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());

    const current = makeDraft({ id: 900, version: 7, subject: '别处的版本' });
    api.updateDraft.mockRejectedValueOnce(new ApiError(409, 'version_conflict', '草稿已在其他窗口修改', { current }));
    fireEvent.change(subjectInput(), { target: { value: 'ab' } });
    const dialog = await screen.findByRole('dialog', { name: '已在其他窗口修改' });
    expect(screen.getAllByText('已在其他窗口修改').length).toBeGreaterThan(1); // dialog + save state

    fireEvent.click(within(dialog).getByRole('button', { name: '覆盖' }));
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(2));
    expect(api.updateDraft.mock.calls[1]![1]).toMatchObject({ subject: 'ab', version: 1, force: true });
    await waitFor(() => expect(screen.queryByRole('dialog', { name: '已在其他窗口修改' })).not.toBeInTheDocument());
  });

  it('重新加载 replaces the form with the server draft', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: 'mine' } });
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());

    const current = makeDraft({ id: 900, version: 7, subject: '别处的版本', html: '<p>别处的正文</p>' });
    api.updateDraft.mockRejectedValueOnce(new ApiError(409, 'version_conflict', '冲突', { current }));
    fireEvent.change(subjectInput(), { target: { value: 'mine 2' } });
    const dialog = await screen.findByRole('dialog', { name: '已在其他窗口修改' });
    fireEvent.click(within(dialog).getByRole('button', { name: '重新加载' }));
    await waitFor(() => expect(subjectInput()).toHaveValue('别处的版本'));
    expect(document.querySelector('.ProseMirror')).toHaveTextContent('别处的正文');

    // Further edits PUT against the reloaded version.
    fireEvent.change(subjectInput(), { target: { value: '别处的版本！' } });
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalledTimes(2));
    expect(api.updateDraft.mock.calls[1]![1]).toMatchObject({ version: 7 });
  });

  it('closing saves pending edits first; a window never edited closes without requests', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '保存并关闭' }));
    expect(windows()).toHaveLength(0);
    expect(api.createDraft).not.toHaveBeenCalled();

    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: '没保存就关' } });
    fireEvent.keyDown(subjectInput(), { key: 'Escape' }); // Esc closes too (saving)
    await waitFor(() => expect(windows()).toHaveLength(0));
    expect(api.createDraft).toHaveBeenCalledTimes(1);
    expect(api.createDraft.mock.calls[0]![0]).toMatchObject({ subject: '没保存就关' });
  });

  it('a failed save on close asks before discarding', async () => {
    api.createDraft.mockRejectedValue(new ApiError(0, 'network_error', '网络连接失败'));
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: 'x' } });
    fireEvent.click(screen.getByRole('button', { name: '保存并关闭' }));
    const dialog = await screen.findByRole('dialog', { name: '无法保存草稿' });
    expect(windows()).toHaveLength(1);
    fireEvent.click(within(dialog).getByRole('button', { name: '仍然关闭' }));
    await waitFor(() => expect(windows()).toHaveLength(0));
  });

  it('discard deletes the draft', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(subjectInput(), { target: { value: 'x' } });
    await waitFor(() => expect(screen.getByText('已保存')).toBeInTheDocument());
    fireEvent.click(screen.getByRole('button', { name: '舍弃草稿' }));
    await waitFor(() => expect(api.deleteDraft).toHaveBeenCalledWith(900));
    expect(windows()).toHaveLength(0);
    expect(toastTexts()).toContain('草稿已舍弃');
  });

  it('shows a missing draft (404) instead of the form', async () => {
    api.getDraft.mockRejectedValue(new ApiError(404, 'not_found', '不存在'));
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'draft', draftId: 5 }));
    expect(await screen.findByText('此草稿已不存在（可能已发送或已删除）')).toBeInTheDocument();
  });
});

describe('ComposeWindow — send', () => {
  it('sends the final fields with the version, closes, toasts 撤销 / 查看邮件, and undo reopens the draft', async () => {
    const { qc } = renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    typeTo('bob@ext.test');
    fireEvent.change(subjectInput(), { target: { value: '你好' } });
    // Ctrl+Enter sends; the draft is created first (prepareSend), then POST send.
    fireEvent.keyDown(subjectInput(), { key: 'Enter', ctrlKey: true });
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    const [draftId, body] = api.sendDraft.mock.calls[0]! as [number, DraftSendRequest];
    expect(draftId).toBe(900);
    expect(body).toMatchObject({ version: 1, scheduled_at: null, draft: { subject: '你好', to: [{ name: '', email: 'bob@ext.test' }] } });
    await waitFor(() => expect(windows()).toHaveLength(0));

    const sent = useToastStore.getState().toasts.find((t) => t.message === '邮件已发送')!;
    expect(sent.actions.map((a) => a.label)).toEqual(['撤销', '查看邮件']);
    expect(sent.durationMs).toBe(5000);

    const restored = makeDraft({ id: 900, version: 3, subject: '你好', to: [{ name: '', email: 'bob@ext.test' }] });
    api.undoSend.mockResolvedValue({ draft: restored });
    await act(async () => sent.actions[0]!.onClick());
    await waitFor(() => expect(api.undoSend).toHaveBeenCalledWith(900));
    await waitFor(() => expect(windows()).toHaveLength(1));
    expect(useComposeStore.getState().windows[0]!.init).toEqual({ kind: 'draft', draftId: 900 });
    expect(qc.getQueryData(queryKeys.draft(900))).toEqual(restored);
    await waitFor(() => expect(subjectInput()).toHaveValue('你好'));
    expect(toastTexts()).toContain('已撤销发送');
  });

  it('Ctrl+Enter in the body sends without inserting a line break', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.com' }], subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    const body = document.querySelector('.ProseMirror')!;
    act(() => void editorOf().commands.insertContent('正文'));
    fireEvent.keyDown(body, { key: 'Enter', ctrlKey: true });
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
    const html = (api.sendDraft.mock.calls[0]![1] as DraftSendRequest).draft!.html!;
    expect(html).toContain('正文');
    expect(html).not.toContain('<br');
  });

  it('omits 撤销 when undo_ms is 0', async () => {
    api.sendDraft.mockResolvedValue(sendResult({ undo_ms: 0 }));
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.com' }], subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    await waitFor(() => expect(toastTexts()).toContain('邮件已发送'));
    expect(useToastStore.getState().toasts.find((t) => t.message === '邮件已发送')!.actions.map((a) => a.label)).toEqual(['查看邮件']);
  });

  it('asks before sending without a subject', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', to: [{ name: '', email: 'a@b.com' }] }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    const dialog = await screen.findByRole('dialog', { name: '在没有主题的情况下发送此邮件？' });
    expect(api.sendDraft).not.toHaveBeenCalled();
    fireEvent.click(within(dialog).getByRole('button', { name: '发送' }));
    await waitFor(() => expect(api.sendDraft).toHaveBeenCalledTimes(1));
  });

  it('blocks sending without valid recipients', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: 's' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    expect(await screen.findByText('请至少指定一位收件人。')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: '确定' }));
    typeTo('not-an-address');
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    expect(await screen.findByText('地址“not-an-address”无法识别。请确认所有地址的格式都正确无误。')).toBeInTheDocument();
    expect(api.sendDraft).not.toHaveBeenCalled();
  });

  it('highlights unknown local recipients returned by the server and keeps the window', async () => {
    api.sendDraft.mockRejectedValue(new ApiError(422, 'unknown_local_recipient', '收件人地址不存在', { emails: ['ghost@az.test'] }));
    renderDock();
    act(() =>
      void useComposeStore.getState().open({ kind: 'new', subject: 's', to: [{ name: '', email: 'Ghost@az.test' }, { name: '', email: 'ok@az.test' }] }),
    );
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    expect(await screen.findByText('以下收件人地址不存在：ghost@az.test')).toBeInTheDocument();
    expect(windows()).toHaveLength(1);
    const bad = document.querySelectorAll('.cw-chip.is-invalid');
    expect(bad).toHaveLength(1);
    expect(bad[0]).toHaveTextContent('Ghost@az.test');
    fireEvent.click(screen.getByRole('button', { name: '确定' }));
    await waitFor(() => expect(screen.getByRole('button', { name: '发送' })).toBeEnabled());
  });

  it('maps send_as_forbidden / too_many_recipients / message_too_large to alerts', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: 's', to: [{ name: '', email: 'a@b.com' }] }));
    await screen.findByRole('textbox', { name: '主题' });
    const cases: [ApiError, string][] = [
      [new ApiError(403, 'send_as_forbidden', '你没有权限以此地址发送邮件'), '你没有权限以此地址发送邮件'],
      [new ApiError(422, 'too_many_recipients', 'x', { field: 'draft.cc' }), '“抄送”栏最多只能填写 50 个地址。'],
      [new ApiError(413, 'message_too_large', '邮件过大（附件总计最大 28 MB）'), '邮件过大（附件总计最大 28 MB）'],
    ];
    for (const [err, text] of cases) {
      api.sendDraft.mockRejectedValueOnce(err);
      fireEvent.click(screen.getByRole('button', { name: '发送' }));
      expect(await screen.findByText(text)).toBeInTheDocument();
      fireEvent.click(screen.getByRole('button', { name: '确定' }));
      await waitFor(() => expect(screen.queryByText(text)).not.toBeInTheDocument());
    }
  });

  it('a 409 on send opens the conflict dialog', async () => {
    api.sendDraft.mockRejectedValue(new ApiError(409, 'version_conflict', '冲突', { current: makeDraft({ id: 900, version: 4 }) }));
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: 's', to: [{ name: '', email: 'a@b.com' }] }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.click(screen.getByRole('button', { name: '发送' }));
    expect(await screen.findByRole('dialog', { name: '已在其他窗口修改' })).toBeInTheDocument();
  });
});

describe('ComposeWindow — attachments', () => {
  it('uploads files with progress, disables send meanwhile, and saves the attachment ids', async () => {
    let finish!: (a: ReturnType<typeof makeAttachment>) => void;
    api.uploadAttachment.mockImplementation(
      (_file, opts) =>
        new Promise((resolve) => {
          opts?.onProgress?.({ loaded: 40, total: 100, fraction: 0.4 });
          finish = resolve;
        }),
    );
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new', subject: 's', to: [{ name: '', email: 'a@b.com' }] }));
    await screen.findByRole('textbox', { name: '主题' });
    const input = screen.getByTestId('compose-attach-input');
    fireEvent.change(input, { target: { files: [fakeFile('报告.pdf', 1000, 'application/pdf')] } });
    expect(await screen.findByRole('progressbar', { name: '正在上传 40%' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '发送' })).toBeDisabled();
    expect(api.uploadAttachment.mock.calls[0]![1]).toMatchObject({ inline: false, filename: '报告.pdf' });

    act(() => finish(makeAttachment({ id: 70, filename: '报告.pdf' })));
    expect(await screen.findByRole('link', { name: '报告.pdf' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: '发送' })).toBeEnabled();
    await waitFor(() => expect(api.createDraft).toHaveBeenCalled());
    expect(api.createDraft.mock.calls.at(-1)![0].attachment_ids).toEqual([70]);

    // Removing it drops the id from the next save.
    fireEvent.click(screen.getByRole('button', { name: '移除附件 报告.pdf' }));
    await waitFor(() => expect(api.updateDraft).toHaveBeenCalled());
    expect(api.updateDraft.mock.calls.at(-1)![1].attachment_ids).toEqual([]);
  });

  it('rejects files over the size limits with a Chinese explanation', async () => {
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(screen.getByTestId('compose-attach-input'), { target: { files: [fakeFile('huge.iso', 26 * 1024 * 1024)] } });
    expect(await screen.findByText('“huge.iso”超过了 25 MB 的单个附件大小上限')).toBeInTheDocument();
    expect(api.uploadAttachment).not.toHaveBeenCalled();
  });

  it('inserts photos inline with data-att-id (never data: URIs)', async () => {
    api.uploadAttachment.mockResolvedValue(
      makeAttachment({ id: 71, filename: 'p.png', content_type: 'image/png', inline: true, view_url: 'https://api.az.test/api/files/71?d=i' }),
    );
    renderDock();
    act(() => void useComposeStore.getState().open({ kind: 'new' }));
    await screen.findByRole('textbox', { name: '主题' });
    fireEvent.change(screen.getByTestId('compose-photo-input'), { target: { files: [fakeFile('p.png', 500, 'image/png')] } });
    // (ProseMirror adds its own `img.ProseMirror-separator` after inline nodes.)
    await waitFor(() => expect(document.querySelector('.ProseMirror img:not(.ProseMirror-separator)')).not.toBeNull());
    const img = document.querySelector('.ProseMirror img:not(.ProseMirror-separator)')!;
    expect(img).toHaveAttribute('data-att-id', '71');
    expect(img.getAttribute('src')).toBe('https://api.az.test/api/files/71?d=i');
    expect(api.uploadAttachment.mock.calls[0]![1]).toMatchObject({ inline: true });
    await waitFor(() => expect(api.createDraft).toHaveBeenCalled());
    const saved = api.createDraft.mock.calls.at(-1)![0];
    expect(saved.attachment_ids).toEqual([71]);
    expect(saved.html).toContain('data-att-id="71"');
    expect(saved.html).not.toContain('data:');
  });
});

describe('ComposeDock', () => {
  it('lays out windows, minimizes to a title-bar chip and maximizes over a backdrop', async () => {
    renderDock();
    let a = '';
    act(() => {
      a = useComposeStore.getState().open({ kind: 'new', subject: '第一封' });
      useComposeStore.getState().open({ kind: 'new', subject: '第二封' });
    });
    await waitFor(() => expect(windows()).toHaveLength(2));
    const first = windows()[0]!;
    expect(within(first).getByRole('heading')).toHaveTextContent('第一封');

    fireEvent.click(within(first).getByRole('button', { name: '最小化' }));
    expect(first).toHaveClass('is-min');
    expect(first.querySelector('.cw-body')).not.toBeVisible();
    fireEvent.click(within(first).getByRole('button', { name: '展开' }));
    expect(first).not.toHaveClass('is-min');

    // The title bar toggles too (pointerdown + click, as in a browser).
    const title = within(first).getByRole('button', { name: '第一封' });
    fireEvent.pointerDown(title);
    fireEvent.click(title);
    expect(first).toHaveClass('is-min');
    fireEvent.pointerDown(title);
    fireEvent.click(title);
    expect(first).not.toHaveClass('is-min');

    fireEvent.click(within(first).getByRole('button', { name: '全屏' }));
    expect(first).toHaveClass('is-max');
    fireEvent.click(screen.getByTestId('compose-backdrop'));
    expect(first).not.toHaveClass('is-max');
    expect(useComposeStore.getState().windows.find((w) => w.key === a)!.maximized).toBe(false);
  });
});
