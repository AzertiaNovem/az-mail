/**
 * One compose window [WP-F] (DESIGN.md §5): Gmail's light title bar (新邮件 / subject;
 * minimize · full screen · close), From (aliases), To / Cc / Bcc chips, subject, TipTap body
 * with the formatting bar, the quoted original behind "…", attachments with progress, and the
 * action row (发送 ▾ 定时发送 · format · attach · link · photo · signature · 已保存 · discard).
 *
 * Behaviour: lazy draft creation on first edit and 1.5 s autosave (useAutosave); close / Esc
 * saves first (Gmail keeps a draft); discard deletes it; drop / paste uploads (images inline,
 * never data: URIs); Ctrl/⌘+Enter sends (useDraftSend); version conflicts offer 重新加载 / 覆盖.
 * Minimized windows stay mounted (title bar only), so the editor keeps its state.
 */
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { useEditorState } from '@tiptap/react';
import { memo, useCallback, useEffect, useId, useLayoutEffect, useRef, useState, type DragEvent, type KeyboardEvent } from 'react';
import { useNavigate } from 'react-router';
import { errorMessage, isApiError } from '@/api/client';
import { deleteDraft, getDraft, getMessage, getThread } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import type {
  Address,
  Attachment,
  Draft,
  DraftInput,
  Me,
  UnknownLocalRecipientDetails,
  VersionConflictDetails,
} from '@/api/types';
import { Button, ConfirmDialog, Dialog, DropdownMenu, IconButton } from '@/components/common';
import { resolveApiUrl } from '@/config';
import { t } from '@/i18n/zh';
import { attachmentIdsForSave, isBlankHtml } from '@/lib/emailHtml';
import { isImeKeyEvent } from '@/lib/keyboard';
import { safeTimeZone } from '@/lib/quote';
import { isValidEmail, normalizeEmail } from '@/lib/recipients';
import { checkUploadSizes, isInlineImage, sizeRejectionMessage, UploadQueue, type UploadItem } from '@/lib/upload';
import { useComposeStore, type ComposeInit, type ComposeWin } from '@/stores/compose';
import { toast } from '@/stores/toast';
import { AttachmentBar } from './AttachmentBar';
import { LoadingWindow, TitleBar, WindowShell, windowClass } from './ComposeShell';
import { dragHasFiles, EditorSurface, filesFromTransfer, useMailEditor, type Editor } from './Editor';
import { EditorToolbar } from './EditorToolbar';
import { hasSignature } from './extensions/Signature';
import { IdentitySelect } from './IdentitySelect';
import { LinkDialog } from './LinkDialog';
import { QuotedToggle } from './QuotedToggle';
import { RecipientField, type RecipientFieldHandle } from './RecipientField';
import { SchedulePicker } from './SchedulePicker';
import { findParent, seedFromDraft, seedFromInit, withUnsaved, type ComposeSeed } from './seed';
import { SendButton } from './SendButton';
import { DraftConflictError, useAutosave, type AutosaveApi } from './useAutosave';
import { fieldLabel, preSendCheck, recipientFieldOf, useDraftSend } from './useDraftSend';
import { useMe } from './useMe';

const FORMATTING_PREF_KEY = 'azmail.compose.formatting';

function readFormattingPref(): boolean {
  try {
    return localStorage.getItem(FORMATTING_PREF_KEY) !== '0';
  } catch {
    return true;
  }
}

function writeFormattingPref(on: boolean): void {
  try {
    localStorage.setItem(FORMATTING_PREF_KEY, on ? '1' : '0');
  } catch {
    /* storage disabled */
  }
}

/** Test seams (autosave API / debounce); production uses the defaults. */
export interface ComposeTestOptions {
  autosaveApi?: AutosaveApi;
  debounceMs?: number;
}

// ───────────── loading ─────────────

type LoadState = { status: 'loading' } | { status: 'error'; message: string; retry?: () => void } | { status: 'ready'; seed: ComposeSeed };

/**
 * What the form starts from. A window whose draft already exists always (re)loads that draft,
 * whatever it was opened as: its form may remount while the window stays open (the app shell
 * unmounts on /login after an expired session), and seeding a fresh message / reply again
 * would show a blank body and create a second draft.
 */
export function effectiveInit(win: Pick<ComposeWin, 'init' | 'draftId'>): ComposeInit {
  return win.draftId !== null && win.init.kind !== 'draft' ? { kind: 'draft', draftId: win.draftId } : win.init;
}

function useComposeSeed(win: ComposeWin, me: Me | undefined): LoadState {
  // The seed is computed once: later cache updates must not reset a form being edited.
  const [frozen, setFrozen] = useState<ComposeSeed | null>(null);
  const init = effectiveInit(win);
  const isReply = init.kind === 'reply' || init.kind === 'reply_all' || init.kind === 'forward';
  const parentId = isReply ? init.parentMessageId : 0;
  const threadId = isReply ? init.threadId : 0;

  const draftQ = useQuery({
    queryKey: queryKeys.draft(init.kind === 'draft' ? init.draftId : 0),
    queryFn: ({ signal }) => getDraft(init.kind === 'draft' ? init.draftId : 0, signal),
    enabled: init.kind === 'draft' && !frozen,
    staleTime: staleTimes.draft,
    retry: false,
  });
  const threadQ = useQuery({
    queryKey: queryKeys.thread(threadId),
    queryFn: ({ signal }) => getThread(threadId, signal),
    enabled: isReply && !frozen,
    staleTime: staleTimes.thread,
  });
  const fromThread = isReply ? findParent(threadQ.data?.messages, parentId) : null;
  const msgQ = useQuery({
    queryKey: ['compose', 'parent', parentId] as const,
    queryFn: ({ signal }) => getMessage(parentId, signal),
    enabled: isReply && !frozen && !fromThread && (threadQ.isError || threadQ.isSuccess),
    staleTime: staleTimes.thread,
  });

  if (frozen) return { status: 'ready', seed: frozen };
  if (!me) return { status: 'loading' };

  let seed: ComposeSeed;
  if (init.kind === 'draft') {
    if (draftQ.data) seed = seedFromDraft(draftQ.data);
    else if (draftQ.isError)
      return {
        status: 'error',
        message: isApiError(draftQ.error, 'not_found') ? t('compose.draftMissing') : errorMessage(draftQ.error),
        retry: isApiError(draftQ.error, 'not_found') ? undefined : () => void draftQ.refetch(),
      };
    else return { status: 'loading' };
  } else if (init.kind === 'new') {
    seed = seedFromInit(init, me, null);
  } else {
    const parent = fromThread ?? msgQ.data ?? null;
    if (parent) seed = seedFromInit(init, me, parent);
    else if (msgQ.isError)
      return {
        status: 'error',
        message: t('compose.loadFailed'),
        retry: () => {
          void threadQ.refetch();
          void msgQ.refetch();
        },
      };
    else return { status: 'loading' };
  }
  seed = withUnsaved(seed, win.unsaved);
  setFrozen(seed); // render-phase update: re-renders right away with the frozen seed
  return { status: 'ready', seed };
}

// ───────────── window ─────────────

export interface ComposeWindowProps {
  win: ComposeWin;
  testOptions?: ComposeTestOptions;
}

export const ComposeWindow = memo(function ComposeWindow({ win, testOptions }: ComposeWindowProps) {
  const me = useMe();
  const load = useComposeSeed(win, me.data);
  // 重新加载 after a conflict remounts the form with the server's draft.
  const [reloaded, setReloaded] = useState<{ draft: Draft; n: number } | null>(null);
  const onReload = useCallback((draft: Draft) => setReloaded((r) => ({ draft, n: (r?.n ?? 0) + 1 })), []);

  if (me.isError && !me.data) {
    return <WindowShell win={win}>{errorMessage(me.error)}</WindowShell>;
  }
  if (!me.data || load.status === 'loading') return <LoadingWindow win={win} />;
  if (load.status === 'error') {
    return (
      <WindowShell win={win}>
        <span>{load.message}</span>
        {load.retry && (
          <Button variant="tonal" size="sm" onClick={load.retry}>
            {t('actions.retry')}
          </Button>
        )}
      </WindowShell>
    );
  }
  const seed = reloaded ? seedFromDraft(reloaded.draft) : load.seed;
  return <ComposeForm key={reloaded?.n ?? 0} win={win} me={me.data} seed={seed} onReload={onReload} testOptions={testOptions} />;
});

// ───────────── form ─────────────

interface Fields {
  fromAddressId: number | undefined;
  to: Address[];
  cc: Address[];
  bcc: Address[];
  subject: string;
  quotedHtml: string | null;
  attachments: Attachment[];
}

interface AlertState {
  title: string;
  lines: string[];
}

type UploadSource = 'attach' | 'photo' | 'paste' | 'drop';

const validOnly = (list: readonly Address[]) => list.filter((a) => isValidEmail(a.email));

/** Same file (a server-side copy of an attachment has a new id). */
const sameFile = (a: Attachment, b: Attachment) => a.filename === b.filename && a.size === b.size && a.content_type === b.content_type;

interface ComposeFormProps {
  win: ComposeWin;
  me: Me;
  seed: ComposeSeed;
  onReload: (draft: Draft) => void;
  testOptions?: ComposeTestOptions;
}

function ComposeForm({ win, me, seed, onReload, testOptions }: ComposeFormProps) {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const titleId = useId();
  const focused = useComposeStore((s) => s.focusedKey === win.key);
  const tz = safeTimeZone(me.settings.timezone);
  const filesOrigins = me.server.files_origins;

  const rootRef = useRef<HTMLElement>(null);
  const toRef = useRef<RecipientFieldHandle>(null);
  const ccRef = useRef<RecipientFieldHandle>(null);
  const bccRef = useRef<RecipientFieldHandle>(null);
  const attachInputRef = useRef<HTMLInputElement>(null);
  const photoInputRef = useRef<HTMLInputElement>(null);
  const editorRef = useRef<Editor | null>(null);
  const lastHtmlRef = useRef(seed.html);
  const editorOwned = useRef(new Set(seed.editorInlineIds));

  const [fields, setFieldsState] = useState<Fields>(() => ({
    fromAddressId: seed.fromAddressId,
    to: seed.to,
    cc: seed.cc,
    bcc: seed.bcc,
    subject: seed.subject,
    quotedHtml: seed.quotedHtml,
    attachments: seed.attachments,
  }));
  const fieldsRef = useRef(fields);
  const [showCc, setShowCc] = useState(seed.cc.length > 0);
  const [showBcc, setShowBcc] = useState(seed.bcc.length > 0);
  const [quoteOpen, setQuoteOpen] = useState(false);
  const [flagged, setFlagged] = useState<ReadonlySet<string>>(() => new Set());
  const [showFormatting, setShowFormatting] = useState(readFormattingPref);
  const [dragging, setDragging] = useState(false);
  const [sending, setSending] = useState(false);
  const [closing, setClosing] = useState(false);
  const [scheduleOpen, setScheduleOpen] = useState(false);
  const [linkOpen, setLinkOpen] = useState(false);
  const [alert, setAlert] = useState<AlertState | null>(null);
  const [confirmSubject, setConfirmSubject] = useState<{ scheduledAt: number | null } | null>(null);
  const [conflict, setConflict] = useState<{ current: Draft | null } | null>(null);
  const [conflictBusy, setConflictBusy] = useState(false);
  const [closeFailed, setCloseFailed] = useState<string | null>(null);

  // A forward shows the parent's attachments until the first save copies them onto the draft;
  // the server's copies are then adopted into `fields.attachments` (see adoptServerAttachments).
  const [inherited, setInherited] = useState<Attachment[]>(seed.inheritedAttachments);
  const inheritedRef = useRef(inherited);
  /** Inherited attachments removed before the copies existed (their copies are dropped on adoption). */
  const removedInherited = useRef<Attachment[]>([]);

  const [queue] = useState(() => new UploadQueue());
  const [uploads, setUploads] = useState<UploadItem[]>([]);
  useEffect(() => {
    const unsubscribe = queue.subscribe(setUploads);
    return () => {
      unsubscribe();
      queue.cancelAll();
    };
  }, [queue]);
  const uploadsPending = uploads.some((u) => u.state === 'queued' || u.state === 'uploading');

  // ── autosave ──
  const collect = useCallback((): DraftInput => {
    const f = fieldsRef.current;
    const ed = editorRef.current;
    const html = ed && !ed.isDestroyed ? ed.getHTML() : lastHtmlRef.current;
    lastHtmlRef.current = html;
    return {
      from_address_id: f.fromAddressId,
      to: validOnly(f.to),
      cc: validOnly(f.cc),
      bcc: validOnly(f.bcc),
      subject: f.subject,
      html,
      quoted_html: f.quotedHtml,
      attachment_ids: attachmentIdsForSave({ attachments: f.attachments, editorHtml: html, editorOwnedInlineIds: editorOwned.current }),
    };
  }, []);
  const createFields = useCallback(
    (): DraftInput => ({
      mode: seed.mode,
      parent_message_id: seed.parentMessageId,
      ...(seed.includeParentAttachments ? { include_parent_attachments: true } : {}),
    }),
    [seed.mode, seed.parentMessageId, seed.includeParentAttachments],
  );
  // POST /api/drafts copied the forwarded message's attachments: show them and keep them on
  // every later save and on the send (an unlisted regular attachment is deleted by the server).
  // fieldsRef is updated synchronously: a send right after the first save collects them too.
  const adoptServerAttachments = (draft: Draft) => {
    const known = new Set(fieldsRef.current.attachments.map((a) => a.id));
    const removed = [...removedInherited.current];
    const added: Attachment[] = [];
    let dropped = false;
    for (const a of draft.attachments) {
      // Copied inline quote images are kept by the server through the quote's references.
      if (a.inline || known.has(a.id)) continue;
      const i = removed.findIndex((r) => sameFile(r, a));
      if (i !== -1) {
        removed.splice(i, 1);
        dropped = true; // removed by the user before the copy existed: the next save drops it
        continue;
      }
      added.push(a);
    }
    removedInherited.current = [];
    if (inheritedRef.current.length) {
      inheritedRef.current = [];
      setInherited([]);
    }
    if (added.length) {
      fieldsRef.current = { ...fieldsRef.current, attachments: [...fieldsRef.current.attachments, ...added] };
      setFieldsState(fieldsRef.current);
    }
    if (dropped) saver.markDirty();
  };

  const saver = useAutosave({
    winKey: win.key,
    draftId: seed.draftId,
    version: seed.version,
    collect,
    createFields,
    onConflict: (current) => setConflict({ current }),
    onSaved: (draft, created) => {
      if (created) adoptServerAttachments(draft);
    },
    api: testOptions?.autosaveApi,
    debounceMs: testOptions?.debounceMs,
  });

  // Restored unsaved edits (the form remounted after a re-login) are saved right away.
  const seedDirty = !!seed.dirty;
  useEffect(() => {
    if (seedDirty) saver.markDirty();
  }, [saver, seedDirty]);

  // The window outlives this form when the app shell unmounts (expired session → /login):
  // stash the edits the server does not have yet, so the remounted form starts from them.
  useEffect(() => {
    const store = useComposeStore.getState();
    if (store.windows.find((w) => w.key === win.key)?.unsaved) store.patch(win.key, { unsaved: undefined });
    const owned = editorOwned.current; // one Set for the form's lifetime (mutated in place)
    return () => {
      const st = useComposeStore.getState();
      if (!st.windows.some((w) => w.key === win.key)) return; // closed / sent / discarded
      if (!saver.dirty && !saver.saving && !saver.inConflict) return;
      const input = collect();
      const f = fieldsRef.current;
      st.patch(win.key, {
        unsaved: {
          fromAddressId: f.fromAddressId,
          to: [...f.to],
          cc: [...f.cc],
          bcc: [...f.bcc],
          subject: f.subject,
          html: input.html ?? lastHtmlRef.current,
          quotedHtml: f.quotedHtml,
          attachments: [...f.attachments],
          editorInlineIds: [...owned],
        },
      });
    };
    // Mount / unmount only (all stable): the stash is read from refs at unmount time.
  }, [saver, win.key, collect]);

  const update = useCallback(
    (patch: Partial<Fields>) => {
      fieldsRef.current = { ...fieldsRef.current, ...patch };
      setFieldsState(fieldsRef.current);
      saver.markDirty();
    },
    [saver],
  );

  // ── editor ──
  const editor = useMailEditor({
    initialHtml: seed.html,
    placeholder: t('compose.bodyPlaceholder'),
    ariaLabel: t('compose.bodyLabel'),
    autofocus: win.minimized ? false : seed.focus === 'body-start' ? 'start' : seed.focus === 'body-end' ? 'end' : false,
    onUpdate: () => saver.markDirty(),
    onPasteFiles: (files) => addFiles(files, 'paste'),
    onDropFiles: (files) => addFiles(files, 'drop'),
  });
  useLayoutEffect(() => {
    editorRef.current = editor;
  }, [editor]);
  useEffect(() => {
    const editable = !sending && !closing;
    // emitUpdate=false: toggling editability is not an edit (it must not create a draft).
    if (editor && !editor.isDestroyed && editor.isEditable !== editable) editor.setEditable(editable, false);
  }, [editor, sending, closing]);

  const send = useDraftSend({ winKey: win.key, saver, collect, timeZone: tz });

  // Title bar follows the subject.
  useEffect(() => {
    const title = fields.subject.trim();
    if (useComposeStore.getState().windows.find((w) => w.key === win.key)?.title !== title) {
      useComposeStore.getState().patch(win.key, { title });
    }
  }, [fields.subject, win.key]);

  // ── uploads ──
  const addFiles = (files: File[], source: UploadSource) => {
    if (files.length === 0 || sending) return;
    const wantInline = source !== 'attach';
    const images = new Set(wantInline ? files.filter(isInlineImage) : []);
    const existing =
      [...fieldsRef.current.attachments, ...inheritedRef.current].reduce((n, a) => n + a.size, 0) + queue.pendingBytes();
    const { accepted, rejected } = checkUploadSizes(files, existing);
    if (rejected.length) setAlert({ title: t('compose.attachments.rejectedTitle'), lines: rejected.map(sizeRejectionMessage) });
    const inline = accepted.filter((f) => images.has(f));
    const regular = accepted.filter((f) => !images.has(f));
    if (inline.length) {
      queue.add(inline, {
        inline: true,
        onDone: (_item, att) => {
          const ed = editorRef.current;
          editorOwned.current.add(att.id);
          update({ attachments: [...fieldsRef.current.attachments, att] });
          if (ed && !ed.isDestroyed) {
            ed.chain()
              .focus()
              .insertAttachmentImage({ src: resolveApiUrl(att.view_url ?? att.download_url), alt: att.filename, attId: att.id })
              .run();
          }
        },
      });
    }
    if (regular.length) {
      queue.add(regular, { inline: false, onDone: (_item, att) => update({ attachments: [...fieldsRef.current.attachments, att] }) });
    }
  };

  const removeAttachment = (id: number) => {
    const fromParent = inheritedRef.current.find((a) => a.id === id);
    if (fromParent) {
      inheritedRef.current = inheritedRef.current.filter((a) => a.id !== id);
      setInherited(inheritedRef.current);
      removedInherited.current = [...removedInherited.current, fromParent];
      saver.markDirty();
      return;
    }
    update({ attachments: fieldsRef.current.attachments.filter((a) => a.id !== id) });
  };

  // ── close / discard ──
  const finishClose = () => {
    saver.dispose();
    queue.cancelAll();
    const id = saver.draftId;
    useComposeStore.getState().close(win.key);
    if (id !== null) {
      qc.removeQueries({ queryKey: queryKeys.draft(id), exact: true });
      void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
      void qc.invalidateQueries({ queryKey: queryKeys.counts() });
    }
  };

  const handleClose = async () => {
    if (closing || sending) return;
    for (const r of [toRef, ccRef, bccRef]) r.current?.commit();
    if (saver.inConflict) {
      setConflict((c) => c ?? { current: null });
      return;
    }
    if (!saver.dirty && !saver.saving) {
      finishClose();
      return;
    }
    setClosing(true);
    try {
      await saver.flush();
      finishClose();
    } catch (e) {
      setClosing(false);
      if (e instanceof DraftConflictError) setConflict({ current: e.current });
      else setCloseFailed(errorMessage(e));
    }
  };

  const handleDiscard = async () => {
    if (sending) return;
    saver.dispose();
    queue.cancelAll();
    await saver.settle();
    const id = saver.draftId;
    useComposeStore.getState().close(win.key);
    if (id === null) {
      toast.push({ message: t('compose.discarded') });
      return;
    }
    try {
      await deleteDraft(id);
      toast.push({ message: t('compose.discarded') });
    } catch (e) {
      if (!isApiError(e, 'not_found')) toast.error(t('compose.discardFailed', { reason: errorMessage(e) }));
    }
    qc.removeQueries({ queryKey: queryKeys.draft(id), exact: true });
    void qc.invalidateQueries({ queryKey: queryKeys.threadsAll() });
    void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  };

  // ── send ──
  const handleSendError = (e: unknown) => {
    if (e instanceof DraftConflictError) {
      setConflict({ current: e.current });
      return;
    }
    if (isApiError(e, 'version_conflict')) {
      const current = (e.detailsAs<VersionConflictDetails>().current as Draft | undefined) ?? null;
      saver.enterConflict(current);
      setConflict({ current });
      return;
    }
    const title = t('compose.errors.title');
    if (isApiError(e, 'unknown_local_recipient')) {
      const emails = (e.detailsAs<UnknownLocalRecipientDetails>().emails ?? []).filter((x) => typeof x === 'string');
      const set = new Set(emails.map(normalizeEmail));
      setFlagged(set);
      if (fieldsRef.current.cc.some((a) => set.has(normalizeEmail(a.email)))) setShowCc(true);
      if (fieldsRef.current.bcc.some((a) => set.has(normalizeEmail(a.email)))) setShowBcc(true);
      setAlert({ title, lines: [emails.length ? t('compose.errors.unknownLocal', { emails: emails.join('、') }) : e.message] });
      return;
    }
    if (isApiError(e, 'too_many_recipients')) {
      const field = recipientFieldOf(e.details.field);
      setAlert({ title, lines: [field ? t('compose.errors.tooMany', { field: fieldLabel(field) }) : e.message] });
      return;
    }
    if (isApiError(e, 'no_recipients')) {
      setAlert({ title, lines: [t('compose.errors.noRecipients')] });
      return;
    }
    setAlert({ title, lines: [errorMessage(e)] });
  };

  const handleSend = async (scheduledAt: number | null = null, subjectConfirmed = false) => {
    if (sending || closing || uploadsPending) return;
    for (const r of [toRef, ccRef, bccRef]) r.current?.commit();
    const f = fieldsRef.current;
    const problem = preSendCheck(f);
    if (problem) {
      const title = t('compose.errors.title');
      if (problem.kind === 'no_recipients') setAlert({ title, lines: [t('compose.errors.noRecipients')] });
      else if (problem.kind === 'invalid_address') setAlert({ title, lines: [t('compose.errors.invalidAddress', { address: problem.address })] });
      else setAlert({ title, lines: [t('compose.errors.tooMany', { field: fieldLabel(problem.field) })] });
      if (problem.kind !== 'no_recipients') {
        if (problem.field === 'cc') setShowCc(true);
        if (problem.field === 'bcc') setShowBcc(true);
      }
      return;
    }
    if (!subjectConfirmed && !f.subject.trim()) {
      setConfirmSubject({ scheduledAt });
      return;
    }
    setSending(true);
    setFlagged(new Set());
    const outcome = await send(scheduledAt);
    if (outcome.ok) return; // the window is closed
    setSending(false);
    saver.resume();
    handleSendError(outcome.error);
  };

  // ── conflict resolution ──
  const reloadFromServer = async () => {
    setConflictBusy(true);
    try {
      const current = conflict?.current ?? (saver.draftId !== null ? await getDraft(saver.draftId) : null);
      if (!current) return;
      qc.setQueryData(queryKeys.draft(current.id), current);
      saver.acceptReload(current);
      saver.dispose();
      queue.cancelAll();
      setConflict(null);
      onReload(current);
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setConflictBusy(false);
    }
  };

  const overwriteServer = async () => {
    setConflictBusy(true);
    try {
      await saver.overwrite();
      setConflict(null);
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setConflictBusy(false);
    }
  };

  // ── drag & drop ──
  useEffect(() => {
    if (!dragging) return;
    const reset = () => setDragging(false);
    window.addEventListener('drop', reset);
    window.addEventListener('dragend', reset);
    return () => {
      window.removeEventListener('drop', reset);
      window.removeEventListener('dragend', reset);
    };
  }, [dragging]);

  const onDragEnter = (e: DragEvent) => {
    if (win.minimized || sending || !dragHasFiles(e.dataTransfer)) return;
    e.preventDefault();
    setDragging(true);
  };

  // ── keyboard ──
  const onKeyDown = (e: KeyboardEvent) => {
    // Events from portaled dialogs bubble through React; only handle our own DOM.
    if (!rootRef.current?.contains(e.target as Node)) return;
    // Esc cancels an IME candidate list and Enter confirms it: neither closes nor sends.
    if (isImeKeyEvent(e)) return;
    const mod = e.ctrlKey || e.metaKey;
    if (mod && e.key === 'Enter') {
      e.preventDefault();
      void handleSend();
      return;
    }
    if (mod && (e.key === 'k' || e.key === 'K') && editor?.isFocused) {
      e.preventDefault();
      setLinkOpen(true);
      return;
    }
    if (e.key === 'Escape' && !e.defaultPrevented) {
      e.preventDefault();
      void handleClose();
    }
  };

  const focusWindow = () => {
    // A minimized chip is expanded by its title-bar click; focusing on pointerdown would
    // expand it first and the click would minimize it again.
    if (win.minimized) return;
    if (useComposeStore.getState().focusedKey !== win.key) useComposeStore.getState().focus(win.key);
  };

  const busy = sending || closing;
  const title = fields.subject.trim() || t('compose.newMessage');
  const saveText =
    win.saveState === 'saving'
      ? t('compose.saveState.saving')
      : win.saveState === 'saved' || (win.saveState === 'dirty' && win.draftId !== null)
        ? t('compose.saveState.saved')
        : win.saveState === 'error'
          ? t('compose.saveState.error')
          : win.saveState === 'conflict'
            ? t('compose.saveState.conflict')
            : '';
  const regularAttachments = [...inherited, ...fields.attachments.filter((a) => !a.inline)];

  return (
    // A non-modal dialog handles its own shortcuts (Esc, Ctrl+Enter), focus, blur-save and drops.
    // oxlint-disable-next-line jsx-a11y/no-noninteractive-element-interactions
    <section
      ref={rootRef}
      className={windowClass(win, focused)}
      role="dialog"
      aria-labelledby={titleId}
      data-testid="compose-window"
      onKeyDown={onKeyDown}
      onPointerDownCapture={focusWindow}
      onBlur={(e) => {
        if (!rootRef.current?.contains(e.relatedTarget as Node | null)) saver.saveSoon();
      }}
      onDragEnter={onDragEnter}
      onDragOver={(e) => {
        if (dragging) e.preventDefault();
      }}
    >
      <TitleBar win={win} titleId={titleId} title={title} onClose={() => void handleClose()} busy={closing} />

      <div className="cw-body" hidden={win.minimized}>
        <div className="cw-fields">
          <IdentitySelect
            identities={me.identities}
            value={fields.fromAddressId}
            onChange={(id) => update({ fromAddressId: id })}
            disabled={busy}
          />
          <RecipientField
            ref={toRef}
            label={t('compose.to')}
            value={fields.to}
            onChange={(to) => update({ to })}
            flagged={flagged}
            autoFocus={seed.focus === 'to' && !win.minimized}
            disabled={busy}
            trailing={
              <span className="cw-ccbcc">
                {!showCc && (
                  <button type="button" onClick={() => setShowCc(true)} aria-label={t('compose.addCc')}>
                    {t('compose.cc')}
                  </button>
                )}
                {!showBcc && (
                  <button type="button" onClick={() => setShowBcc(true)} aria-label={t('compose.addBcc')}>
                    {t('compose.bcc')}
                  </button>
                )}
              </span>
            }
          />
          {showCc && (
            <RecipientField ref={ccRef} label={t('compose.cc')} value={fields.cc} onChange={(cc) => update({ cc })} flagged={flagged} disabled={busy} />
          )}
          {showBcc && (
            <RecipientField
              ref={bccRef}
              label={t('compose.bcc')}
              value={fields.bcc}
              onChange={(bcc) => update({ bcc })}
              flagged={flagged}
              disabled={busy}
            />
          )}
          <div className="cw-field">
            <input
              className="cw-subject"
              aria-label={t('compose.subject')}
              placeholder={t('compose.subject')}
              value={fields.subject}
              disabled={busy}
              maxLength={998}
              onChange={(e) => update({ subject: e.target.value })}
            />
          </div>
        </div>

        <div className="cw-scroll">
          <EditorSurface editor={editor} />
          {fields.quotedHtml && !isBlankHtml(fields.quotedHtml) && (
            <QuotedToggle
              html={fields.quotedHtml}
              expanded={quoteOpen}
              onToggle={() => setQuoteOpen((o) => !o)}
              onRemove={() => {
                setQuoteOpen(false);
                update({ quotedHtml: null });
              }}
              allowRemote={me.settings.remote_images === 'always'}
              filesOrigins={filesOrigins}
            />
          )}
          <AttachmentBar
            attachments={regularAttachments}
            uploads={uploads}
            onRemove={removeAttachment}
            onCancelUpload={(id) => queue.cancel(id)}
            disabled={busy}
          />
        </div>

        {showFormatting && <EditorToolbar editor={editor} className="cw-format-bar" />}

        <footer className="cw-actions">
          <SendButton
            onSend={() => void handleSend()}
            onSchedule={() => setScheduleOpen(true)}
            disabled={uploadsPending || closing}
            disabledReason={uploadsPending ? t('compose.sendDisabledUploading') : undefined}
            sending={sending}
          />
          <div className="cw-action-icons">
            <IconButton
              icon="format_color_text"
              label={t('compose.formatting')}
              size="sm"
              tooltipSide="top"
              active={showFormatting}
              onClick={() => {
                setShowFormatting((v) => {
                  writeFormattingPref(!v);
                  return !v;
                });
              }}
            />
            <IconButton icon="attach_file" label={t('compose.attach')} size="sm" tooltipSide="top" disabled={busy} onClick={() => attachInputRef.current?.click()} />
            <IconButton icon="link" label={t('compose.insertLink')} size="sm" tooltipSide="top" disabled={busy} onClick={() => setLinkOpen(true)} />
            <IconButton icon="photo" label={t('compose.insertPhoto')} size="sm" tooltipSide="top" disabled={busy} onClick={() => photoInputRef.current?.click()} />
            <SignatureMenu
              editor={editor}
              signatureHtml={me.settings.signature_html}
              filesOrigins={filesOrigins}
              disabled={busy}
              onManage={() => void navigate('/settings/general')}
            />
          </div>
          <span className="cw-save-state" aria-live="polite">
            {saveText}
          </span>
          <IconButton icon="delete" label={t('compose.discard')} size="sm" tooltipSide="top" disabled={sending} onClick={() => void handleDiscard()} />
        </footer>

        {dragging && (
          <div
            className="cw-drop"
            onDragOver={(e) => {
              e.preventDefault();
              e.dataTransfer.dropEffect = 'copy';
            }}
            onDragLeave={(e) => {
              if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setDragging(false);
            }}
            onDrop={(e) => {
              e.preventDefault();
              setDragging(false);
              addFiles(filesFromTransfer(e.dataTransfer), 'drop');
            }}
          >
            <span className="cw-drop-title">{t('compose.dropHere')}</span>
            <span className="cw-drop-hint">{t('compose.dropHint')}</span>
          </div>
        )}
      </div>

      <input
        ref={attachInputRef}
        type="file"
        multiple
        hidden
        data-testid="compose-attach-input"
        onChange={(e) => {
          addFiles(Array.from(e.target.files ?? []), 'attach');
          e.target.value = '';
        }}
      />
      <input
        ref={photoInputRef}
        type="file"
        accept="image/*"
        multiple
        hidden
        data-testid="compose-photo-input"
        onChange={(e) => {
          addFiles(Array.from(e.target.files ?? []), 'photo');
          e.target.value = '';
        }}
      />

      <SchedulePicker open={scheduleOpen} onOpenChange={setScheduleOpen} timeZone={tz} onSchedule={(at) => void handleSend(at)} />
      <LinkDialog editor={editor} open={linkOpen} onOpenChange={setLinkOpen} />

      <Dialog
        open={alert !== null}
        onOpenChange={(o) => !o && setAlert(null)}
        title={alert?.title ?? ''}
        size="sm"
        showClose={false}
        description={
          alert && (
            <div className="flex flex-col gap-1">
              {alert.lines.map((l, i) => (
                <p key={i}>{l}</p>
              ))}
            </div>
          )
        }
        footer={
          <Button onClick={() => setAlert(null)} autoFocus>
            {t('compose.errors.ok')}
          </Button>
        }
      />

      <ConfirmDialog
        open={confirmSubject !== null}
        onOpenChange={(o) => !o && setConfirmSubject(null)}
        title={t('compose.emptySubject.title')}
        confirmLabel={t('compose.emptySubject.confirm')}
        onConfirm={() => {
          const at = confirmSubject?.scheduledAt ?? null;
          setConfirmSubject(null);
          void handleSend(at, true);
        }}
      />

      <Dialog
        open={conflict !== null}
        onOpenChange={() => {
          /* must choose */
        }}
        dismissible={false}
        showClose={false}
        title={t('compose.conflict.title')}
        description={t('compose.conflict.message')}
        size="sm"
        footer={
          <>
            <Button variant="text" onClick={() => void overwriteServer()} disabled={conflictBusy}>
              {t('compose.conflict.overwrite')}
            </Button>
            <Button onClick={() => void reloadFromServer()} loading={conflictBusy} autoFocus>
              {t('compose.conflict.reload')}
            </Button>
          </>
        }
      />

      <ConfirmDialog
        open={closeFailed !== null}
        onOpenChange={(o) => !o && setCloseFailed(null)}
        title={t('compose.closeFailed.title')}
        message={t('compose.closeFailed.message', { reason: closeFailed ?? '' })}
        confirmLabel={t('compose.closeFailed.confirm')}
        cancelLabel={t('compose.closeFailed.cancel')}
        danger
        onConfirm={() => {
          setCloseFailed(null);
          finishClose();
        }}
      />
    </section>
  );
}

// ───────────── signature menu ─────────────

/**
 * 签名 ▸ 不使用签名 / 插入签名. The client owns the signature (the server sends the body as
 * the editor shows it): new messages, replies and forwards start with it when enabled, and this
 * menu removes it or (re-)inserts exactly one copy above the quote.
 */
function SignatureMenu({
  editor,
  signatureHtml,
  filesOrigins,
  disabled,
  onManage,
}: {
  editor: Editor | null;
  signatureHtml: string;
  filesOrigins: string[];
  disabled?: boolean;
  onManage: () => void;
}) {
  const present = useEditorState({ editor, selector: ({ editor: e }) => hasSignature(e) }) ?? false;
  const blank = isBlankHtml(signatureHtml);
  return (
    <DropdownMenu
      side="top"
      aria-label={t('compose.signature')}
      trigger={<IconButton icon="signature" label={t('compose.signature')} size="sm" tooltipSide="top" disabled={disabled || !editor} />}
      items={[
        {
          key: 'none',
          label: t('compose.signatureMenu.none'),
          icon: present ? undefined : 'check',
          onSelect: () => editor?.chain().focus().removeSignature().run(),
        },
        {
          key: 'insert',
          label: blank ? t('compose.signatureMenu.empty') : t('compose.signatureMenu.insert'),
          icon: present ? 'check' : undefined,
          disabled: blank,
          onSelect: () => editor?.chain().focus().setSignature(signatureHtml, filesOrigins).run(),
        },
        { key: 'sep', type: 'separator' },
        { key: 'manage', label: t('compose.signatureMenu.manage'), icon: 'settings', onSelect: onManage },
      ]}
    />
  );
}
