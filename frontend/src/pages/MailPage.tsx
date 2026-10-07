/**
 * Mail page [WP-E]: resolves the route into a list view and an optional thread.
 *
 *   /mail/:folder[/:threadId]          folder (validated against FOLDER_IDS)
 *   /mail/label/:labelId[/:threadId]   label (must exist once labels are loaded)
 *   /mail/search[/:threadId]?q=        search (empty q → inbox)
 *
 * The list stays mounted (hidden) while a thread is open, so returning keeps its scroll
 * position and the thread view knows its neighbours for j / k.
 */
import { useEffect, useMemo } from 'react';
import { Navigate, useParams, useSearchParams } from 'react-router';
import { FOLDER_IDS, type FolderId } from '@/api/types';
import { useCounts, useLabels, useMe } from '@/components/mail/queries';
import { ThreadList } from '@/components/mail/ThreadList';
import { ThreadView } from '@/components/mail/ThreadView';
import { folderView, labelView, searchView, type ListView } from '@/components/mail/view';
import { folderName, t } from '@/i18n/zh';
import { NotFound } from './NotFound';

export type MailPageKind = 'folder' | 'label' | 'search';

const isFolder = (v: string | undefined): v is FolderId => (FOLDER_IDS as readonly string[]).includes(v ?? '');

/** Positive integer id from a route param, else null. */
export function parseId(v: string | undefined): number | null {
  if (!v || !/^\d{1,15}$/.test(v)) return null;
  const n = Number(v);
  return n > 0 ? n : null;
}

function useListTitle(view: ListView | null, labelName: string | undefined) {
  const me = useMe();
  const counts = useCounts().data;
  useEffect(() => {
    if (!view || !me) return;
    const name =
      view.kind === 'folder' && view.folder ? folderName(view.folder) : view.kind === 'label' ? (labelName ?? '') : t('mail.search.title');
    const unread = view.folder === 'inbox' ? (counts?.inbox_unread ?? 0) : 0;
    document.title =
      unread > 0
        ? t('mail.documentTitle.folderUnread', { name, count: unread, email: me.email })
        : t('mail.documentTitle.folder', { name, email: me.email });
  }, [view, me, counts?.inbox_unread, labelName]);
}

export function MailPage({ kind }: { kind: MailPageKind }) {
  const params = useParams();
  const [search] = useSearchParams();
  const labelsQuery = useLabels();
  const q = kind === 'search' ? (search.get('q') ?? '').trim() : '';
  const folder = kind === 'folder' ? params.folder : undefined;
  const labelId = kind === 'label' ? parseId(params.labelId) : null;
  const threadParam = params.threadId;
  const threadId = threadParam === undefined ? null : parseId(threadParam);
  const label = labelId !== null ? labelsQuery.data?.find((l) => l.id === labelId) : undefined;

  const view = useMemo<ListView | null>(() => {
    if (kind === 'folder') return isFolder(folder) ? folderView(folder) : null;
    if (kind === 'label') return labelId !== null ? labelView(labelId) : null;
    return q ? searchView(q) : null;
  }, [kind, folder, labelId, q]);

  useListTitle(threadId === null ? view : null, label?.name);

  if (kind === 'search' && !q) return <Navigate to="/mail/inbox" replace />;
  if (!view) return <NotFound embedded />;
  if (threadParam !== undefined && threadId === null) return <NotFound embedded />;
  if (kind === 'label' && labelsQuery.isSuccess && !label) return <NotFound embedded />;

  const backLabel =
    view.kind === 'search'
      ? t('mail.thread.backToSearch')
      : view.kind === 'label'
        ? t('mail.thread.backToLabel', { name: label?.name ?? '' })
        : t('mail.thread.back', { folder: folderName(view.folder ?? 'inbox') });

  return (
    <div className="h-full">
      <div className={threadId === null ? 'h-full' : 'hidden'}>
        <ThreadList key={view.basePath + view.search} view={view} active={threadId === null} />
      </div>
      {threadId !== null && <ThreadView key={threadId} threadId={threadId} view={view} backLabel={backLabel} />}
    </div>
  );
}
