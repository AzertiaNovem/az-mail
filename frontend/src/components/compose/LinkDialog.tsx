/**
 * Insert / edit link dialog [WP-F] for the compose body and the signature editor.
 * Accepts http(s) URLs, bare domains ("example.com/x" → https://…) and e-mail addresses
 * (→ mailto:). Other schemes (javascript:, data:, …) are rejected.
 */
import { getMarkRange } from '@tiptap/core';
import { useState, type FormEvent } from 'react';
import { Button, Dialog, TextField } from '@/components/common';
import { t } from '@/i18n/zh';
import { isValidEmail } from '@/lib/recipients';
import type { Editor } from './Editor';

/** Normalized href for user input, or null when it is not an acceptable link. */
export function normalizeLinkUrl(input: string): string | null {
  const s = input.trim();
  if (!s || /\s/.test(s)) return null;
  if (/^mailto:/i.test(s)) return isValidEmail(s.slice(7).split('?')[0] ?? '') ? s : null;
  if (/^https?:\/\//i.test(s)) {
    try {
      const u = new URL(s);
      return u.hostname ? s : null;
    } catch {
      return null;
    }
  }
  if (isValidEmail(s)) return `mailto:${s}`;
  if (/^[a-z][a-z0-9+.-]*:/i.test(s) && !/^[^:]+:\d/.test(s)) return null; // other schemes
  if (/^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+(:\d+)?([/?#].*)?$/.test(s)) return `https://${s}`;
  return null;
}

export interface LinkDialogProps {
  editor: Editor | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

interface Initial {
  text: string;
  url: string;
  editing: boolean;
  from: number;
  to: number;
}

/** The selection to link (widened to the whole link when the caret is inside one), read without dispatching. */
function readSelection(editor: Editor | null): Initial {
  if (!editor) return { text: '', url: '', editing: false, from: 0, to: 0 };
  const { state } = editor;
  let { from, to } = state.selection;
  const href = (editor.getAttributes('link').href as string | undefined) ?? '';
  const linkType = state.schema.marks.link;
  if (href && linkType) {
    const range = getMarkRange(state.doc.resolve(from), linkType);
    if (range) ({ from, to } = range);
  }
  return { text: state.doc.textBetween(from, to, ' '), url: href, editing: !!href, from, to };
}

export function LinkDialog({ editor, open, onOpenChange }: LinkDialogProps) {
  // Remount the form each time the dialog opens so it reads the current selection.
  return open ? <LinkForm editor={editor} onOpenChange={onOpenChange} /> : null;
}

function LinkForm({ editor, onOpenChange }: { editor: Editor | null; onOpenChange: (open: boolean) => void }) {
  const [initial] = useState(() => readSelection(editor));
  const [text, setText] = useState(initial.text);
  const [url, setUrl] = useState(initial.url);
  const [error, setError] = useState<string | null>(null);

  const apply = (e?: FormEvent) => {
    e?.preventDefault();
    if (!editor) return;
    const href = normalizeLinkUrl(url);
    if (!href) {
      setError(t('compose.link.invalidUrl'));
      return;
    }
    const label = text.trim() || url.trim();
    const size = editor.state.doc.content.size;
    const from = Math.min(initial.from, size);
    const to = Math.min(initial.to, size);
    const chain = editor.chain().focus().setTextSelection({ from, to });
    if (from !== to && label === initial.text) {
      chain.setLink({ href }).run();
    } else {
      chain
        .insertContent({ type: 'text', text: label, marks: [{ type: 'link', attrs: { href } }] })
        .unsetMark('link')
        .run();
    }
    onOpenChange(false);
  };

  const remove = () => {
    editor?.chain().focus().setTextSelection({ from: initial.from, to: initial.to }).unsetLink().run();
    onOpenChange(false);
  };

  return (
    <Dialog
      open
      onOpenChange={onOpenChange}
      title={initial.editing ? t('compose.link.editTitle') : t('compose.link.insertTitle')}
      size="md"
      footer={
        <>
          {initial.editing && (
            <Button variant="text" danger onClick={remove} className="mr-auto">
              {t('compose.link.remove')}
            </Button>
          )}
          <Button variant="text" onClick={() => onOpenChange(false)}>
            {t('actions.cancel')}
          </Button>
          <Button onClick={() => apply()} disabled={!url.trim()}>
            {t('compose.link.apply')}
          </Button>
        </>
      }
    >
      <form className="flex flex-col gap-4" onSubmit={apply} noValidate>
        <TextField label={t('compose.link.text')} value={text} onChange={(e) => setText(e.target.value)} />
        <TextField
          label={t('compose.link.url')}
          value={url}
          placeholder={t('compose.link.urlPlaceholder')}
          autoFocus
          inputMode="url"
          error={error ?? undefined}
          onChange={(e) => {
            setUrl(e.target.value);
            setError(null);
          }}
        />
        <button type="submit" hidden aria-hidden="true" tabIndex={-1} />
      </form>
    </Dialog>
  );
}
