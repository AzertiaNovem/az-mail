/**
 * The list view a mail route shows [WP-E]: folder (`/mail/:folder`), label
 * (`/mail/label/:labelId`) or search (`/mail/search?q=`).
 */
import type { ThreadListFilter } from '@/api/queryKeys';
import { threadListFilter } from '@/api/queryKeys';
import type { FolderId } from '@/api/types';

export interface ListView {
  kind: 'folder' | 'label' | 'search';
  filter: ThreadListFilter;
  /** Folder of a folder view, else null. */
  folder: FolderId | null;
  /** Label of a label view, else null. */
  labelId: number | null;
  /** List URL without the thread segment. */
  basePath: string;
  /** Query string kept on thread URLs ("?q=…" for search). */
  search: string;
}

export function folderView(folder: FolderId): ListView {
  return { kind: 'folder', filter: threadListFilter({ folder }), folder, labelId: null, basePath: `/mail/${folder}`, search: '' };
}

export function labelView(labelId: number): ListView {
  return { kind: 'label', filter: threadListFilter({ labelId }), folder: null, labelId, basePath: `/mail/label/${labelId}`, search: '' };
}

export function searchView(q: string): ListView {
  return {
    kind: 'search',
    filter: threadListFilter({ q }),
    folder: null,
    labelId: null,
    basePath: '/mail/search',
    search: `?q=${encodeURIComponent(q)}`,
  };
}

/** URL of a thread inside a view. */
export function threadPath(view: Pick<ListView, 'basePath' | 'search'>, threadId: number): string {
  return `${view.basePath}/${threadId}${view.search}`;
}

/** URL of the list itself. */
export function listPath(view: Pick<ListView, 'basePath' | 'search'>): string {
  return `${view.basePath}${view.search}`;
}
