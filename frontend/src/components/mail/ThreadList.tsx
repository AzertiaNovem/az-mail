/**
 * Thread list [WP-E]: toolbar + Gmail pager (cursor stack), rows, empty / loading / error
 * states, optimistic actions and the list keyboard shortcuts (j/k o/Enter x s e # ! Shift+I/U).
 */
import { useQueryClient } from '@tanstack/react-query';
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type MouseEvent } from 'react';
import { useNavigate } from 'react-router';
import { queryKeys } from '@/api/queryKeys';
import type { FolderId, Label, ThreadAction, ThreadListItem } from '@/api/types';
import { Button, ConfirmDialog, Icon } from '@/components/common';
import { useIsMobile } from '@/components/layout/useMediaQuery';
import { t, type MessageKey } from '@/i18n/zh';
import { useShortcuts } from '@/lib/keyboard';
import { listScope, selectPageCursor, selectPageIndex, selectSelection, useUiStore } from '@/stores/ui';
import { openDraftThread } from './compose';
import { EmptyState } from './EmptyState';
import { ListSkeleton } from './ListSkeleton';
import { ListToolbar, selectIds, type SelectKind } from './ListToolbar';
import type { MoveTarget } from './MoveToMenu';
import { useLabels, usePageSize, useThreadList, useTimeZone } from './queries';
import { moveThreads, performThreadAction } from './threadActions';
import { ThreadRow } from './ThreadRow';
import { useNow } from './useNow';
import { threadPath, type ListView } from './view';

const EMPTY_ICONS: Record<FolderId, string> = {
  inbox: 'inbox',
  starred: 'star',
  scheduled: 'schedule_send',
  sent: 'send',
  drafts: 'draft',
  all: 'stacked_email',
  spam: 'report',
  trash: 'delete',
};

function emptyText(view: ListView): { icon: string; title: string; hint?: string } {
  if (view.kind === 'search') return { icon: 'search', title: t('mail.search.empty'), hint: t('mail.search.emptyHint') };
  if (view.kind === 'label') return { icon: 'label', title: t('mail.list.empty.label'), hint: t('mail.list.empty.labelHint') };
  const f = view.folder ?? 'inbox';
  const hint = t(`mail.list.empty.${f}Hint` as MessageKey);
  return { icon: EMPTY_ICONS[f], title: t(`mail.list.empty.${f}` as MessageKey), hint: hint || undefined };
}

/** Plain-click check: modified clicks (new tab / window) keep the link's default behaviour. */
const isPlainClick = (e: MouseEvent) => e.button === 0 && !e.metaKey && !e.ctrlKey && !e.shiftKey && !e.altKey;

/** `active` = visible (shortcuts only fire for the visible list; it stays mounted under an open thread). */
export function ThreadList({ view, active = true }: { view: ListView; active?: boolean }) {
  const qc = useQueryClient();
  const navigate = useNavigate();
  const pageSize = usePageSize();
  const scope = listScope(view.filter);
  const cursor = useUiStore(selectPageCursor(scope));
  const pageIndex = useUiStore(selectPageIndex(scope));
  const selected = useUiStore(selectSelection(scope));
  const cursorId = useUiStore((s) => s.cursorId);
  const density = useUiStore((s) => s.density);
  const { data, isPending, isError, isFetching, isPlaceholderData, refetch } = useThreadList(view.filter, cursor, pageSize);
  const labels = useLabels().data;
  const tz = useTimeZone();
  const now = useNow();
  const mobile = useIsMobile();
  const listRef = useRef<HTMLDivElement>(null);
  // Scroll offset while visible; restored when the list is shown again after a thread.
  const scrollPos = useRef(0);
  const [confirmDelete, setConfirmDelete] = useState<number[] | null>(null);

  const items = useMemo(() => data?.items ?? [], [data]);
  const ids = useMemo(() => items.map((i) => i.id), [items]);
  const labelsById = useMemo(() => new Map<number, Label>((labels ?? []).map((l) => [l.id, l])), [labels]);
  const offset = pageIndex * pageSize;

  // 每页显示 changed (here or in settings): the cursor stacks and every cached page were built
  // with the old size, so offsets and rows would no longer match. Start over at page 1.
  useLayoutEffect(() => {
    if (useUiStore.getState().syncPageSize(pageSize)) void qc.resetQueries({ queryKey: queryKeys.threadsAll() });
  }, [pageSize, qc]);

  // Publish the visible page for the thread view (j/k, "第 3 封，共 50 封") and keep the
  // selection to visible rows.
  useEffect(() => {
    if (!data) return;
    const st = useUiStore.getState();
    st.setList({ scope, ids, basePath: view.basePath, search: view.search, offset, total: data.total });
    st.pruneSelection(scope, ids);
  }, [data, ids, scope, view.basePath, view.search, offset]);

  // A page emptied by actions (or a stale cursor): step back.
  useEffect(() => {
    if (data && items.length === 0 && pageIndex > 0 && !isFetching) useUiStore.getState().popPage(scope);
  }, [data, items.length, pageIndex, isFetching, scope]);

  useEffect(() => {
    scrollPos.current = 0;
    listRef.current?.scrollTo?.({ top: 0 });
  }, [cursor, scope]);

  // display:none drops the scroll offset; put it back when the list is visible again.
  useLayoutEffect(() => {
    if (active && listRef.current) listRef.current.scrollTop = scrollPos.current;
  }, [active]);

  // Latest values for the stable row callbacks.
  const latest = useRef({ items, selected, view });
  useLayoutEffect(() => {
    latest.current = { items, selected, view };
  });

  /** Draft-only threads open in compose (DESIGN §5); if that fails or it is not one, open the thread. */
  const openDraftOrThread = useCallback(
    (id: number) => {
      const fallback = () => void navigate(threadPath(latest.current.view, id));
      void openDraftThread(qc, id)
        .then((opened) => {
          if (!opened) fallback();
        })
        .catch(fallback);
    },
    [qc, navigate],
  );

  const onOpen = useCallback(
    (item: ThreadListItem, e: MouseEvent<HTMLAnchorElement>) => {
      useUiStore.getState().setCursor(item.id);
      if (item.message_count === 0 && item.draft_count > 0 && isPlainClick(e)) {
        e.preventDefault();
        openDraftOrThread(item.id);
      }
    },
    [openDraftOrThread],
  );

  const openThread = useCallback(
    (item: ThreadListItem) => {
      useUiStore.getState().setCursor(item.id);
      if (item.message_count === 0 && item.draft_count > 0) openDraftOrThread(item.id);
      else void navigate(threadPath(latest.current.view, item.id));
    },
    [openDraftOrThread, navigate],
  );

  const onSelect = useCallback(
    (item: ThreadListItem, checked: boolean, shiftKey: boolean) => {
      const st = useUiStore.getState();
      if (shiftKey) st.selectRange(scope, latest.current.items.map((i) => i.id), item.id, checked);
      else st.toggleSelected(scope, item.id, checked);
    },
    [scope],
  );

  const runAction = useCallback(
    (action: ThreadAction, targetIds: number[]) => {
      if (targetIds.length === 0) return;
      if (action === 'delete_forever') {
        setConfirmDelete(targetIds);
        return;
      }
      void performThreadAction(qc, { ids: targetIds, action }, { toast: true, undo: action === 'read' || action === 'unread' ? false : undefined });
    },
    [qc],
  );

  const onStar = useCallback(
    (item: ThreadListItem) => void performThreadAction(qc, { ids: [item.id], action: item.starred ? 'unstar' : 'star' }),
    [qc],
  );

  const onRowAction = useCallback((item: ThreadListItem, action: ThreadAction) => runAction(action, [item.id]), [runAction]);

  const onBulkSelect = (kind: SelectKind) => useUiStore.getState().setSelection(scope, selectIds(items, kind));

  const onMove = (target: MoveTarget) => {
    const dest =
      target.kind === 'label' ? { kind: 'label' as const, labelId: target.label.id, name: target.label.name } : target;
    void moveThreads(qc, [...selected], dest, { folder: view.folder, labelId: view.labelId });
  };

  const onLabelToggle = (label: Label, add: boolean) =>
    void performThreadAction(
      qc,
      { ids: [...selected], action: add ? 'add_label' : 'remove_label', labelId: label.id },
      { toast: true, labelName: label.name },
    );

  const refresh = () => {
    void refetch();
    void qc.invalidateQueries({ queryKey: queryKeys.counts() });
  };

  // ── keyboard ──
  const targets = (): number[] => (selected.length ? [...selected] : cursorId !== null && ids.includes(cursorId) ? [cursorId] : []);
  const moveCursor = (delta: number) => {
    if (!ids.length) return;
    const i = cursorId === null ? -1 : ids.indexOf(cursorId);
    const next = i === -1 ? (delta > 0 ? 0 : ids.length - 1) : Math.min(ids.length - 1, Math.max(0, i + delta));
    const id = ids[next]!;
    useUiStore.getState().setCursor(id);
    listRef.current?.querySelector(`[data-thread-id="${id}"]`)?.scrollIntoView?.({ block: 'nearest' });
  };
  const cursorItem = () => items.find((i) => i.id === cursorId);
  useShortcuts({
    next: () => moveCursor(1),
    prev: () => moveCursor(-1),
    open: () => {
      const item = cursorItem();
      if (item) openThread(item);
    },
    select: () => {
      if (cursorId !== null && ids.includes(cursorId)) useUiStore.getState().toggleSelected(scope, cursorId);
    },
    star: () => {
      const ts = targets();
      if (!ts.length) return;
      const anyUnstarred = items.some((i) => ts.includes(i.id) && !i.starred);
      void performThreadAction(qc, { ids: ts, action: anyUnstarred ? 'star' : 'unstar' });
    },
    archive: () => {
      if (view.folder === 'trash' || view.folder === 'spam') return;
      runAction('archive', targets());
    },
    delete: () => runAction(view.folder === 'trash' || view.folder === 'spam' ? 'delete_forever' : 'trash', targets()),
    spam: () => runAction(view.folder === 'spam' ? 'not_spam' : 'spam', targets()),
    markRead: () => runAction('read', targets()),
    markUnread: () => runAction('unread', targets()),
  }, active);

  let body;
  if (isPending) body = <ListSkeleton />;
  else if (isError && !data)
    body = (
      <div className="flex flex-col items-center gap-3 py-16 text-on-surface-variant" role="alert">
        <Icon name="error" size={32} className="text-error" />
        <p>{t('mail.list.loadFailed')}</p>
        <Button variant="tonal" icon="refresh" onClick={() => void refetch()}>
          {t('mail.list.retry')}
        </Button>
      </div>
    );
  else if (items.length === 0) {
    const e = emptyText(view);
    body = <EmptyState icon={e.icon} title={e.title} hint={e.hint} />;
  } else
    body = (
      <div role="list" aria-label={t('mail.list.label')}>
        {items.map((item) => (
          <ThreadRow
            key={item.id}
            item={item}
            href={threadPath(view, item.id)}
            folder={view.folder}
            currentLabelId={view.labelId}
            labelsById={labelsById}
            selected={selected.includes(item.id)}
            focused={cursorId === item.id}
            now={now}
            tz={tz}
            mobile={mobile}
            density={density}
            onOpen={onOpen}
            onSelect={onSelect}
            onStar={onStar}
            onAction={onRowAction}
          />
        ))}
      </div>
    );

  const notice =
    view.folder === 'trash' ? t('mail.list.trashNotice') : view.folder === 'spam' ? t('mail.list.spamNotice') : null;

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ListToolbar
        items={items}
        selectedIds={selected}
        folder={view.folder}
        currentLabelId={view.labelId}
        fetching={isFetching && !isPending}
        onSelect={onBulkSelect}
        onRefresh={refresh}
        onAction={(a) => runAction(a, [...selected])}
        onMove={onMove}
        onLabelToggle={onLabelToggle}
        onMarkAllRead={() =>
          void performThreadAction(
            qc,
            { ids: items.filter((i) => i.unread).map((i) => i.id), action: 'read' },
            { toast: true, undo: false },
          )
        }
        pager={{
          offset,
          count: items.length,
          total: data?.total ?? null,
          hasNewer: pageIndex > 0,
          // While the next page loads, `data` is still the previous page (placeholder): its
          // next_cursor is the one just pushed, so a second click must not push it again.
          hasOlder: !!data?.next_cursor && !isPlaceholderData,
          onNewer: () => useUiStore.getState().popPage(scope),
          onOlder: () => {
            if (data?.next_cursor && !isPlaceholderData && data.next_cursor !== cursor) useUiStore.getState().pushPage(scope, data.next_cursor);
          },
        }}
      />
      {notice && items.length > 0 && (
        <div className="mx-4 mb-1 rounded-lg bg-surface-container-low px-4 py-2 text-center text-xs text-on-surface-variant">{notice}</div>
      )}
      <div
        ref={listRef}
        className="min-h-0 flex-1 overflow-y-auto"
        onScroll={(e) => {
          if (active) scrollPos.current = e.currentTarget.scrollTop;
        }}
      >
        {body}
      </div>
      <ConfirmDialog
        open={confirmDelete !== null}
        onOpenChange={(o) => {
          if (!o) setConfirmDelete(null);
        }}
        title={t('mail.actions.deleteForeverTitle', { count: confirmDelete?.length ?? 0 })}
        message={t('mail.actions.deleteForeverMessage')}
        confirmLabel={t('mail.actions.deleteForever')}
        danger
        onConfirm={() => {
          const ids2 = confirmDelete ?? [];
          setConfirmDelete(null);
          void performThreadAction(qc, { ids: ids2, action: 'delete_forever' }, { toast: true });
        }}
      />
    </div>
  );
}
