/**
 * Rich-text editor for mail bodies and signatures [WP-F] (TipTap v3, DESIGN.md §5):
 * StarterKit (incl. Link and Underline), Image with `data-att-id` (AttachmentImage),
 * TextStyle + Color + FontSize, Placeholder, plus local TextAlign / Indent / Signature nodes.
 *
 * Pasted / dropped image FILES are handed to the owner (`onPasteFiles` / `onDropFiles`), which
 * uploads them and inserts `<img data-att-id>`; `data:` images in pasted HTML are dropped by the
 * Image schema, so the body never carries base64 images (§1 E3).
 */
import { Extension } from '@tiptap/core';
import { EditorContent, useEditor, type Editor } from '@tiptap/react';
import { Placeholder } from '@tiptap/extension-placeholder';
import { Color, FontSize, TextStyle } from '@tiptap/extension-text-style';
import StarterKit from '@tiptap/starter-kit';
import { cx } from '@/components/common';
import { AttachmentImage } from './extensions/AttachmentImage';
import { Indent } from './extensions/Indent';
import { Signature } from './extensions/Signature';
import { TextAlign } from './extensions/TextAlign';
import { useLatest } from './useLatest';

export type { Editor } from '@tiptap/react';

/**
 * Ctrl/⌘+Enter belongs to the compose window (send). StarterKit's HardBreak binds it to a line
 * break, which would land in the sent body; claim it first (the key event still bubbles to the
 * window, which sends). Shift+Enter keeps inserting a line break.
 */
const ModEnterPassthrough = Extension.create({
  name: 'modEnterPassthrough',
  priority: 1000,
  addKeyboardShortcuts() {
    return { 'Mod-Enter': () => true };
  },
});

export function createMailExtensions(placeholder: string) {
  return [
    ModEnterPassthrough,
    StarterKit.configure({
      heading: { levels: [1, 2, 3] },
      codeBlock: false,
      // No automatic empty paragraph after a trailing signature: that normalization would
      // count as an edit and create a draft nobody typed.
      trailingNode: false,
      link: {
        openOnClick: false,
        autolink: true,
        linkOnPaste: true,
        defaultProtocol: 'https',
        HTMLAttributes: { target: '_blank', rel: 'noopener noreferrer nofollow' },
      },
    }),
    TextStyle,
    Color,
    FontSize,
    AttachmentImage,
    TextAlign,
    Indent,
    Signature,
    Placeholder.configure({ placeholder }),
  ];
}

/** Files from a DataTransfer (paste / drop); directories and strings are skipped. */
export function filesFromTransfer(dt: DataTransfer | null | undefined): File[] {
  if (!dt) return [];
  const out: File[] = [];
  if (dt.items && dt.items.length) {
    for (const item of Array.from(dt.items)) {
      if (item.kind !== 'file') continue;
      const f = item.getAsFile();
      if (f) out.push(f);
    }
    if (out.length) return out;
  }
  return Array.from(dt.files ?? []);
}

/** Whether a drag carries files (vs. text being dragged inside the editor). */
export const dragHasFiles = (dt: DataTransfer | null | undefined): boolean =>
  !!dt && Array.from(dt.types ?? []).includes('Files');

export interface MailEditorOptions {
  initialHtml: string;
  placeholder: string;
  /** Accessible name of the editable region. */
  ariaLabel: string;
  autofocus?: boolean | 'start' | 'end';
  onUpdate?: (editor: Editor) => void;
  onBlur?: () => void;
  /** Image files pasted without accompanying text (screenshots, copied images). */
  onPasteFiles?: (files: File[]) => void;
  /** Files dropped onto the editor. */
  onDropFiles?: (files: File[]) => void;
  /** Extra class on the contenteditable element. */
  contentClassName?: string;
}

/**
 * Creates the TipTap editor once (options are read at creation; callbacks are always the
 * latest via refs, so callers may pass inline functions).
 */
export function useMailEditor(opts: MailEditorOptions): Editor | null {
  const latest = useLatest(opts);

  return useEditor({
    extensions: createMailExtensions(opts.placeholder),
    content: opts.initialHtml,
    autofocus: opts.autofocus ?? false,
    immediatelyRender: true,
    shouldRerenderOnTransaction: false,
    editorProps: {
      attributes: {
        class: cx('azm-editor-content', opts.contentClassName),
        'aria-label': opts.ariaLabel,
        'aria-multiline': 'true',
        role: 'textbox',
        spellcheck: 'true',
      },
      handlePaste: (_view, event) => {
        const handler = latest.current.onPasteFiles;
        const dt = event.clipboardData;
        if (!handler || !dt) return false;
        // Copying from Word / web pages also puts a rendered image on the clipboard: paste the
        // HTML then. Only image-only clipboards (screenshots, "copy image") become uploads.
        if ((dt.getData('text/plain') ?? '').trim() !== '') return false;
        const files = filesFromTransfer(dt);
        if (files.length === 0) return false;
        event.preventDefault();
        handler(files);
        return true;
      },
      handleDrop: (_view, event, _slice, moved) => {
        if (moved) return false;
        const handler = latest.current.onDropFiles;
        const files = filesFromTransfer(event.dataTransfer);
        if (!handler || files.length === 0) return false;
        event.preventDefault();
        handler(files);
        return true;
      },
    },
    // Only user edits count: updates caused solely by appended (normalization) transactions,
    // e.g. after the autofocus selection change, are not edits.
    onUpdate: ({ editor, transaction }) => {
      if (transaction.docChanged) latest.current.onUpdate?.(editor);
    },
    onBlur: () => latest.current.onBlur?.(),
  });
}

export function EditorSurface({ editor, className }: { editor: Editor | null; className?: string }) {
  return <EditorContent editor={editor} className={cx('azm-editor', className)} />;
}
