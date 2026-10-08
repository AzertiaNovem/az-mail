/**
 * Thread view [WP-E]: toolbar, subject + label chips, stacked messages (older read ones
 * collapsed, "显示 N 封较早的邮件" for long threads, last + unread expanded), drafts as "草稿"
 * rows that open compose, delivery status, attachments, and the 回复 / 回复全部 / 转发 bar.
 * Opening the thread marks it read immediately (optimistic), and so does a message that arrives
 * while the thread is on screen; a draft-only thread opens its draft in compose instead.
 * Like Gmail, the newest message is always expanded — also a reply just sent from here, whose
 * id was already known as a draft. Keyboard: u j k e # ! s Shift+I Shift+U r a f.
 */
import { useQueryClient } from '@tanstack/react-query';
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { useNavigate } from 'react-router';
import { errorMessage, isApiError } from '@/api/client';
import { updateSettings } from '@/api/endpoints';
import { queryKeys } from '@/api/queryKeys';
import type { Attachment, FolderId, Label, Me, Message, ThreadAction, ThreadDetail } from '@/api/types';
import { Button, ConfirmDialog, Icon, IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import type { MailtoParts } from '@/lib/emailFrame';
import { useShortcuts } from '@/lib/keyboard';
import { displaySubject } from '@/lib/subject';
import { toast } from '@/stores/toast';
import { useUiStore } from '@/stores/ui';
import { replyAllCount } from './addressing';
import { AttachmentPreview } from './AttachmentPreview';
import { draftOnlyTarget, openDraft, openMailto, openReply, replyTarget, type ReplyKind } from './compose';
import { LabelChip } from './LabelChip';
import { ThreadSkeleton } from './ListSkeleton';
import { MessageItem } from './MessageItem';
import type { MoveTarget } from './MoveToMenu';
import { useLabels, useMe, useMyAddresses, useThread, useTimeZone } from './queries';
import { moveThreads, patchMessageOptimistic, performThreadAction } from './threadActions';
import { ThreadToolbar } from './ThreadToolbar';
import { useNow } from './useNow';
import { listPath, threadPath, type ListView } from './view';

/** Whether a message belongs to what this view shows (trash shows trashed mail, spam spam). */
export function inViewScope(m: Message, folder: FolderId | null): boolean {
  if (folder === 'trash') return m.trashed;
  if (folder === 'spam') return m.is_spam && !m.trashed;
  return !m.trashed && !m.is_spam;
}

/** Messages to show and how many are hidden by the view scope. */
export function partitionMessages(
  messages: readonly Message[],
  folder: FolderId | null,
  showHidden: boolean,
): { visible: Message[]; hiddenCount: number; hiddenKind: 'nonTrashed' | 'trashed' | 'spam' | null } {
  const inScope = messages.filter((m) => inViewScope(m, folder));
  // A thread reached from search may have nothing in scope: show everything rather than nothing.
  if (showHidden || inScope.length === 0) return { visible: [...messages], hiddenCount: 0, hiddenKind: null };
  const hidden = messages.length - inScope.length;
  if (hidden === 0) return { visible: inScope, hiddenCount: 0, hiddenKind: null };
  const hiddenKind = folder === 'trash' ? 'nonTrashed' : messages.some((m) => m.trashed) ? 'trashed' : 'spam';
  return { visible: inScope, hiddenCount: hidden, hiddenKind };
}

/** Indices [start, end] of the run of collapsed messages hidden behind "显示 N 封较早的邮件". */
export function olderGroup(messages: readonly Message[], expanded: ReadonlySet<number>): [number, number] | null {
  const n = messages.length;
  if (n < 5) return null;
  let end = 0;
  for (let i = 1; i <= n - 3; i++) {
    const m = messages[i]!;
    if (m.is_draft || expanded.has(m.id)) break;
    end = i;
  }
  return end >= 2 ? [1, end] : null;
}

/** Initial expansion: unread messages and the newest non-draft message. */
export function initialExpanded(messages: readonly Message[]): Set<number> {
  const nonDraft = messages.filter((m) => !m.is_draft);
  const out = new Set(nonDraft.filter((m) => !m.is_read).map((m) => m.id));
  const last = nonDraft[nonDraft.length - 1];
  if (last) out.add(last.id);
  return out;
}

/**
 * Messages that became visible as sent / received mail since the last detail: ids never seen as
 * non-drafts. A draft that is sent keeps its id (the row turns into the sent message), so ids are
 * tracked as "seen as non-draft" — a draft id, or a message that went back to being a draft
 * (undo send), counts as fresh once it shows up as mail. `seen` is updated in place.
 */
export function freshMessages(messages: readonly Message[], seen: Set<number>): Message[] {
  const fresh: Message[] = [];
  for (const m of messages) {
    if (m.is_draft) seen.delete(m.id);
    else if (!seen.has(m.id)) {
      seen.add(m.id);
      fresh.push(m);
    }
  }
  return fresh;
}

const LEAVES_VIEW: ReadonlySet<ThreadAction> = new Set(['archive', 'trash', 'spam', 'not_spam', 'restore', 'delete_forever', 'unread']);
const QUIET: ReadonlySet<ThreadAction> = new Set(['star', 'unstar', 'read', 'unread']);

export interface ThreadViewProps {
  threadId: number;
  view: ListView;
  /** "返回收件箱" etc. */
  backLabel: string;
}

export function ThreadView({ threadId, view, backLabel }: ThreadViewProps) {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const me = useMe();
  const mine = useMyAddresses();
  const tz = useTimeZone();
  const now = useNow();
  const labels = useLabels().data;
  const { data: detail, isPending, isError, error, refetch } = useThread(threadId);
  const list = useUiStore((s) => s.list);

  const [showHidden, setShowHidden] = useState(false);
  const [expanded, setExpanded] = useState<Set<number>>(() => new Set());
  const [revealOlder, setRevealOlder] = useState(false);
  const [imagesShown, setImagesShown] = useState<Set<number>>(() => new Set());
  const [trustBusy, setTrustBusy] = useState(false);
  const [preview, setPreview] = useState<Attachment | null>(null);
  const [confirmDelete, setConfirmDelete] = useState(false);
  /** Ids seen as sent / received mail (not drafts); null until the first detail. */
  const seenAsMail = useRef<Set<number> | null>(null);
  const readWhenVisible = useRef(false);
  const markedRead = useRef(false);
  const openedDraft = useRef(false);

  const { visible, hiddenCount, hiddenKind } = useMemo(
    () => partitionMessages(detail?.messages ?? [], view.folder, showHidden),
    [detail, view.folder, showHidden],
  );
  const nonDraft = useMemo(() => visible.filter((m) => !m.is_draft), [visible]);
  const target = replyTarget(visible);
  const labelsById = useMemo(() => new Map<number, Label>((labels ?? []).map((l) => [l.id, l])), [labels]);

  useEffect(() => {
    useUiStore.getState().setCursor(threadId);
  }, [threadId]);

  // Expansion: initial state once, then auto-expand messages that newly appear as mail (arrived,
  // or just sent from a draft of this thread) when unread or the newest one. A fresh unread
  // message in this view is shown, so it is marked read (Gmail).
  useEffect(() => {
    if (!detail) return;
    if (seenAsMail.current === null) {
      seenAsMail.current = new Set();
      freshMessages(detail.messages, seenAsMail.current);
      setExpanded(initialExpanded(visible));
      return;
    }
    const fresh = freshMessages(detail.messages, seenAsMail.current);
    if (!fresh.length) return;
    const last = nonDraft[nonDraft.length - 1];
    setExpanded((prev) => {
      const next = new Set(prev);
      for (const m of fresh) if (!m.is_read || m.id === last?.id) next.add(m.id);
      return next;
    });
    if (fresh.some((m) => !m.is_read && inViewScope(m, view.folder))) {
      if (document.visibilityState === 'hidden') readWhenVisible.current = true;
      else void performThreadAction(qc, { ids: [threadId], action: 'read' });
    }
  }, [detail, visible, nonDraft, qc, threadId, view.folder]);

  // Mail that arrived while the tab was in the background is marked read when it is shown.
  useEffect(() => {
    const onVisible = () => {
      if (document.visibilityState !== 'visible' || !readWhenVisible.current) return;
      readWhenVisible.current = false;
      const d = qc.getQueryData<ThreadDetail>(queryKeys.thread(threadId));
      if (d?.messages.some((m) => !m.is_draft && !m.is_read && inViewScope(m, view.folder)))
        void performThreadAction(qc, { ids: [threadId], action: 'read' });
    };
    document.addEventListener('visibilitychange', onVisible);
    return () => document.removeEventListener('visibilitychange', onVisible);
  }, [qc, threadId, view.folder]);

  // Opening a thread marks it read immediately.
  useEffect(() => {
    if (!detail || markedRead.current) return;
    markedRead.current = true;
    if (detail.messages.some((m) => !m.is_draft && !m.is_read && inViewScope(m, view.folder)))
      void performThreadAction(qc, { ids: [threadId], action: 'read' });
  }, [detail, qc, threadId, view.folder]);

  // A thread that only holds drafts opens the newest draft in compose.
  useEffect(() => {
    if (!detail || openedDraft.current) return;
    const draft = draftOnlyTarget(detail);
    if (!draft) return;
    openedDraft.current = true;
    openDraft(draft.id);
    void navigate(listPath(view), { replace: true });
  }, [detail, navigate, view]);

  useEffect(() => {
    if (detail) document.title = `${displaySubject(detail.subject)} - AZ Mail`;
  }, [detail]);

  // ── neighbours in the list (j / k, "第 3 封，共 50 封") ──
  const ctx = list && list.basePath === view.basePath && list.search === view.search ? list : null;
  const idx = ctx ? ctx.ids.indexOf(threadId) : -1;
  const newerId = ctx && idx > 0 ? (ctx.ids[idx - 1] ?? null) : null;
  const olderId = ctx && idx >= 0 && idx < ctx.ids.length - 1 ? (ctx.ids[idx + 1] ?? null) : null;
  const position = ctx && idx >= 0 ? { index: ctx.offset + idx + 1, total: ctx.total } : null;

  const back = () => void navigate(listPath(view));
  const goTo = (id: number | null) => {
    if (id !== null) void navigate(threadPath(view, id));
  };

  const inInbox = nonDraft.some((m) => m.in_inbox);
  const starred = nonDraft.some((m) => m.is_starred);

  const act = (action: ThreadAction) => {
    if (action === 'delete_forever') {
      setConfirmDelete(true);
      return;
    }
    void performThreadAction(
      qc,
      { ids: [threadId], action },
      { toast: !QUIET.has(action), onUndone: () => void navigate(threadPath(view, threadId)) },
    );
    if (LEAVES_VIEW.has(action)) back();
  };

  const onMove = (m: MoveTarget) => {
    const dest = m.kind === 'label' ? { kind: 'label' as const, labelId: m.label.id, name: m.label.name } : m;
    void moveThreads(qc, [threadId], dest, { folder: view.folder, labelId: view.labelId });
    back();
  };

  const onLabelToggle = (label: Label, add: boolean) =>
    void performThreadAction(
      qc,
      { ids: [threadId], action: add ? 'add_label' : 'remove_label', labelId: label.id },
      { toast: true, labelName: label.name },
    );

  const reply = (kind: ReplyKind, m: Message | null = target) => {
    if (m) openReply(kind, m);
  };

  const trustSender = async (m: Message) => {
    if (!me) return;
    const email = m.from.email.trim().toLowerCase();
    const current = me.settings.trusted_image_senders ?? [];
    if (current.some((e) => e.toLowerCase() === email)) return;
    setTrustBusy(true);
    try {
      const settings = await updateSettings({ trusted_image_senders: [...current, email] });
      qc.setQueryData<Me>(queryKeys.me(), (old) => (old ? { ...old, settings } : old));
      toast.push({ message: t('mail.images.trusted', { sender: email }) });
    } catch (e) {
      toast.error(errorMessage(e));
    } finally {
      setTrustBusy(false);
    }
  };

  const markUnreadFromHere = async (m: Message) => {
    const from = nonDraft.filter((x) => x.date >= m.date);
    const results = await Promise.all(from.map((x) => patchMessageOptimistic(qc, threadId, x.id, { is_read: false })));
    if (results.every(Boolean)) toast.push({ message: t('mail.thread.markedUnreadFromHere') });
    back();
  };

  useShortcuts({
    back,
    next: () => goTo(olderId),
    prev: () => goTo(newerId),
    archive: () => {
      if (view.folder !== 'trash' && view.folder !== 'spam' && inInbox) act('archive');
    },
    delete: () => act(view.folder === 'trash' || view.folder === 'spam' ? 'delete_forever' : 'trash'),
    spam: () => act(view.folder === 'spam' ? 'not_spam' : 'spam'),
    star: () => act(starred ? 'unstar' : 'star'),
    markRead: () => act('read'),
    markUnread: () => act('unread'),
    reply: () => reply('reply'),
    replyAll: () => reply('reply_all'),
    forward: () => reply('forward'),
  });

  if (isPending) return <ThreadSkeleton />;
  if (isError || !detail) {
    const notFound = isApiError(error) && error.status === 404;
    return (
      <div className="flex h-full flex-col">
        <div className="flex h-12 items-center px-2 sm:px-4">
          <IconButton icon="arrow_back" label={backLabel} onClick={back} />
        </div>
        <div className="flex flex-col items-center gap-3 py-16 text-on-surface-variant" role="alert">
          <Icon name={notFound ? 'search_off' : 'error'} size={36} className={notFound ? undefined : 'text-error'} />
          <p>{notFound ? t('mail.thread.notFound') : t('mail.thread.loadFailed')}</p>
          <div className="flex gap-2">
            {!notFound && (
              <Button variant="tonal" icon="refresh" onClick={() => void refetch()}>
                {t('mail.list.retry')}
              </Button>
            )}
            <Button variant="text" onClick={back}>
              {t('mail.thread.backToList')}
            </Button>
          </div>
        </div>
      </div>
    );
  }

  const group = revealOlder ? null : olderGroup(visible, expanded);
  const allExpanded = nonDraft.length > 0 && nonDraft.every((m) => expanded.has(m.id));
  const threadLabels = detail.label_ids.map((id) => labelsById.get(id)).filter((l): l is Label => l !== undefined);
  const filesOrigins = me?.server?.files_origins ?? [];
  const trusted = new Set((me?.settings?.trusted_image_senders ?? []).map((s) => s.toLowerCase()));
  const suspicious = (m: Message) => m.is_spam || m.warnings.length > 0;
  const allowRemote = (m: Message) =>
    imagesShown.has(m.id) ||
    (!suspicious(m) && (m.direction === 'out' || me?.settings?.remote_images === 'always' || trusted.has(m.from.email.toLowerCase())));
  const onlyOne = nonDraft.length === 1;
  const onMailto = (p: MailtoParts) => openMailto(p);
  const toggle = (id: number) =>
    setExpanded((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const hiddenText =
    hiddenKind === 'nonTrashed'
      ? t('mail.thread.nonTrashedHidden', { count: hiddenCount })
      : hiddenKind === 'trashed'
        ? t('mail.thread.trashedHidden', { count: hiddenCount })
        : t('mail.thread.spamHidden', { count: hiddenCount });

  const rendered: ReactNode[] = [];
  visible.forEach((m, i) => {
    if (group && i >= group[0] && i <= group[1]) {
      if (i === group[0]) {
        const count = group[1] - group[0] + 1;
        rendered.push(
          <button
            key="older"
            type="button"
            onClick={() => setRevealOlder(true)}
            className="azm-older-messages group flex w-full items-center gap-3 border-b border-divider px-4 py-1.5 sm:px-6"
            aria-label={t('mail.thread.olderMessages', { count })}
          >
            <span className="flex size-10 shrink-0 items-center justify-center rounded-full border border-outline-variant bg-surface-container text-sm text-on-surface-variant group-hover:bg-hover">
              {count}
            </span>
            <span className="text-sm text-on-surface-variant group-hover:text-on-surface">{t('mail.thread.olderMessages', { count })}</span>
          </button>,
        );
      }
      return;
    }
    rendered.push(
      <MessageItem
        key={m.id}
        message={m}
        expanded={expanded.has(m.id)}
        mine={mine}
        now={now}
        tz={tz}
        filesOrigins={filesOrigins}
        allowRemote={allowRemote(m)}
        canTrustSender={!suspicious(m) && m.direction === 'in'}
        trustBusy={trustBusy}
        replyAll={replyAllCount(m, mine) > 1}
        canDelete={onlyOne}
        onToggle={() => toggle(m.id)}
        onOpenDraft={() => openDraft(m.id)}
        onReply={(kind) => reply(kind, m)}
        onStar={() => void patchMessageOptimistic(qc, threadId, m.id, { is_starred: !m.is_starred })}
        onDelete={() => act('trash')}
        onMarkUnreadFromHere={() => void markUnreadFromHere(m)}
        onShowImages={() => setImagesShown((s) => new Set(s).add(m.id))}
        onTrustSender={() => void trustSender(m)}
        onNotSpam={() => act('not_spam')}
        onPreview={setPreview}
        onMailto={onMailto}
      />,
    );
  });

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ThreadToolbar
        view={view}
        target={{ id: threadId, label_ids: detail.label_ids }}
        backLabel={backLabel}
        inInbox={inInbox}
        starred={starred}
        position={position}
        hasNewer={newerId !== null}
        hasOlder={olderId !== null}
        onBack={back}
        onAction={act}
        onMove={onMove}
        onLabelToggle={onLabelToggle}
        onNewer={() => goTo(newerId)}
        onOlder={() => goTo(olderId)}
      />
      <div className="min-h-0 flex-1 overflow-y-auto pb-8">
        <div className="flex items-start gap-3 px-4 pb-3 pt-2 sm:pl-[76px] sm:pr-6">
          <h1 className="min-w-0 flex-1 text-[22px] leading-8 text-on-surface">
            <span className="mr-2 break-words">{displaySubject(detail.subject)}</span>
            <span className="inline-flex translate-y-[-3px] flex-wrap gap-1 align-middle">
              {inInbox && view.folder !== 'trash' && view.folder !== 'spam' && (
                <LabelChip name={t('mail.list.inbox')} size="md" onRemove={() => act('archive')} removeLabel={t('mail.thread.removeInbox')} />
              )}
              {threadLabels.map((l) => (
                <LabelChip key={l.id} name={l.name} color={l.color} size="md" onRemove={() => onLabelToggle(l, false)} />
              ))}
            </span>
          </h1>
          {nonDraft.length > 1 && (
            <IconButton
              icon={allExpanded ? 'unfold_less' : 'unfold_more'}
              label={allExpanded ? t('mail.thread.collapseAll') : t('mail.thread.expandAll')}
              onClick={() => {
                if (allExpanded) setExpanded(new Set(target ? [target.id] : []));
                else {
                  setRevealOlder(true);
                  setExpanded(new Set(nonDraft.map((m) => m.id)));
                }
              }}
            />
          )}
        </div>
        {hiddenCount > 0 && (
          <p className="px-4 pb-2 text-[13px] text-on-surface-variant sm:pl-[76px]">
            {hiddenText}{' '}
            <button type="button" className="font-medium text-link hover:underline" onClick={() => setShowHidden(true)}>
              {t('mail.thread.trashedShow')}
            </button>
          </p>
        )}
        {showHidden && (
          <p className="px-4 pb-2 text-[13px] sm:pl-[76px]">
            <button type="button" className="font-medium text-link hover:underline" onClick={() => setShowHidden(false)}>
              {t('mail.thread.trashedHide')}
            </button>
          </p>
        )}
        <div className="border-t border-divider">{rendered}</div>
        {target && (
          <div className="flex flex-wrap gap-2 px-4 pt-5 sm:pl-[76px]">
            <Button variant="outlined" icon="reply" onClick={() => reply('reply')}>
              {t('mail.thread.reply')}
            </Button>
            {replyAllCount(target, mine) > 1 && (
              <Button variant="outlined" icon="reply_all" onClick={() => reply('reply_all')}>
                {t('mail.thread.replyAll')}
              </Button>
            )}
            <Button variant="outlined" icon="forward" onClick={() => reply('forward')}>
              {t('mail.thread.forward')}
            </Button>
          </div>
        )}
      </div>
      <AttachmentPreview attachment={preview} onClose={() => setPreview(null)} />
      <ConfirmDialog
        open={confirmDelete}
        onOpenChange={setConfirmDelete}
        title={t('mail.actions.deleteForeverTitle', { count: 1 })}
        message={t('mail.actions.deleteForeverMessage')}
        confirmLabel={t('mail.actions.deleteForever')}
        danger
        onConfirm={() => {
          setConfirmDelete(false);
          void performThreadAction(qc, { ids: [threadId], action: 'delete_forever' }, { toast: true });
          back();
        }}
      />
    </div>
  );
}
