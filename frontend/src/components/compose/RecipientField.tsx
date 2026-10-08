/**
 * To / Cc / Bcc field [WP-F]: address chips (avatar initial, red when invalid or flagged by the
 * server), free typing with separators (`,` `;` `，` `；` Enter Tab blur), pasting whole lists
 * ("a@b, 张三 <c@d>"), Backspace removing the last chip, double-click to edit a chip, and an
 * autocomplete dropdown from GET /api/contacts (team members first, then aliases, then
 * contacts) with keyboard navigation (↑ ↓ Enter Tab Esc). ARIA combobox pattern.
 */
import { useQuery } from '@tanstack/react-query';
import {
  forwardRef,
  useEffect,
  useId,
  useImperativeHandle,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type ClipboardEvent,
  type KeyboardEvent,
  type ReactNode,
} from 'react';
import { searchContacts } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import type { Address, Contact, ContactKind } from '@/api/types';
import { Avatar, cx, Icon } from '@/components/common';
import { t } from '@/i18n/zh';
import { isImeKeyEvent } from '@/lib/keyboard';
import {
  addressLabel,
  containsEmail,
  displayAddress,
  formatAddress,
  hasSeparator,
  isValidEmail,
  mergeAddresses,
  normalizeEmail,
  parseAddressList,
} from '@/lib/recipients';

const KIND_RANK: Record<ContactKind, number> = { team: 0, alias: 1, contact: 2 };

/** Team members first, then aliases, then other contacts; server order within a kind. */
export function sortContacts(items: readonly Contact[]): Contact[] {
  return items
    .map((c, i) => ({ c, i }))
    .sort((a, b) => (KIND_RANK[a.c.kind] ?? 3) - (KIND_RANK[b.c.kind] ?? 3) || a.i - b.i)
    .map((x) => x.c);
}

function useDebounced<T>(value: T, ms: number): T {
  const [v, setV] = useState(value);
  useEffect(() => {
    const h = setTimeout(() => setV(value), ms);
    return () => clearTimeout(h);
  }, [value, ms]);
  return v;
}

export interface RecipientFieldHandle {
  /** Turns pending typed text into chips; returns the resulting list. */
  commit(): Address[];
  focus(): void;
}

export interface RecipientFieldProps {
  label: string;
  value: Address[];
  onChange: (next: Address[]) => void;
  /** Normalized emails flagged by the server (422 unknown_local_recipient). */
  flagged?: ReadonlySet<string>;
  autoFocus?: boolean;
  disabled?: boolean;
  /** Right side of the row (抄送 / 密送 toggles). */
  trailing?: ReactNode;
  /** Debounce of the contacts lookup (ms). */
  debounceMs?: number;
}

export const RecipientField = forwardRef<RecipientFieldHandle, RecipientFieldProps>(function RecipientField(
  { label, value, onChange, flagged, autoFocus, disabled, trailing, debounceMs = 150 },
  ref,
) {
  const inputId = useId();
  const listId = `${inputId}-list`;
  const inputRef = useRef<HTMLInputElement>(null);
  const [text, setText] = useState('');
  const [focused, setFocused] = useState(false);
  const [highlight, setHighlight] = useState(0);
  const [dismissed, setDismissed] = useState(false);

  // Latest values for imperative calls made in the same tick as a change (Ctrl+Enter, blur):
  // the handlers below also write them synchronously.
  const valueRef = useRef(value);
  const textRef = useRef(text);
  useLayoutEffect(() => {
    valueRef.current = value;
    textRef.current = text;
  });

  const commitText = (raw: string): Address[] => {
    const parsed = parseAddressList(raw);
    setText('');
    textRef.current = '';
    if (parsed.length === 0) return valueRef.current;
    const next = mergeAddresses(valueRef.current, parsed);
    valueRef.current = next;
    onChange(next);
    return next;
  };

  useImperativeHandle(ref, () => ({
    commit: () => (textRef.current.trim() ? commitText(textRef.current) : valueRef.current),
    focus: () => inputRef.current?.focus(),
  }));

  const q = useDebounced(text.trim(), debounceMs);
  const enabled = focused && q.length > 0 && !disabled;
  const contacts = useQuery({
    queryKey: queryKeys.contacts(q),
    queryFn: ({ signal }) => searchContacts(q, 8, signal),
    staleTime: staleTimes.contacts,
    enabled,
  });

  const suggestions = useMemo(() => {
    if (!enabled || !contacts.data) return [];
    return sortContacts(contacts.data.items)
      .filter((c) => !containsEmail(value, c.email))
      .slice(0, 8);
  }, [enabled, contacts.data, value]);

  const open = suggestions.length > 0 && text.trim().length > 0 && !dismissed;
  const active = open ? Math.min(highlight, suggestions.length - 1) : -1;

  const choose = (c: Contact) => {
    const next = mergeAddresses(valueRef.current, [{ name: c.name, email: c.email }]);
    valueRef.current = next;
    onChange(next);
    setText('');
    setHighlight(0);
    inputRef.current?.focus();
  };

  const removeAt = (i: number) => {
    const next = valueRef.current.filter((_, j) => j !== i);
    valueRef.current = next;
    onChange(next);
    inputRef.current?.focus();
  };

  const editAt = (i: number) => {
    const a = valueRef.current[i];
    if (!a) return;
    const pending = textRef.current.trim();
    const next = valueRef.current.filter((_, j) => j !== i);
    valueRef.current = next;
    onChange(next);
    setText(pending ? `${pending}, ${formatAddress(a)}` : formatAddress(a));
    requestAnimationFrame(() => inputRef.current?.focus());
  };

  const onInput = (raw: string) => {
    setDismissed(false);
    setHighlight(0);
    if (hasSeparator(raw)) {
      // Commit everything up to the last separator; keep the tail being typed.
      const idx = Math.max(...[',', ';', '，', '；', '、', '\n', '\t'].map((s) => raw.lastIndexOf(s)));
      const head = raw.slice(0, idx);
      const tail = raw.slice(idx + 1);
      if (parseAddressList(head).length) commitText(head);
      setText(tail.trimStart());
      return;
    }
    setText(raw);
  };

  const onKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    // The Enter / Tab / Backspace of an IME composition (Pinyin…) belongs to the input method.
    if (isImeKeyEvent(e)) return;
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      if (text.trim()) commitText(text); // then let the window send
      return;
    }
    if (open && e.key === 'ArrowDown') {
      e.preventDefault();
      setHighlight((h) => (h + 1) % suggestions.length);
      return;
    }
    if (open && e.key === 'ArrowUp') {
      e.preventDefault();
      setHighlight((h) => (h - 1 + suggestions.length) % suggestions.length);
      return;
    }
    if (e.key === 'Enter' || e.key === 'Tab') {
      if (open && active >= 0) {
        e.preventDefault();
        choose(suggestions[active]!);
        return;
      }
      if (text.trim()) {
        if (e.key === 'Enter') e.preventDefault();
        commitText(text);
      } else if (e.key === 'Enter') {
        e.preventDefault();
      }
      return;
    }
    if (e.key === 'Escape' && open) {
      e.preventDefault(); // keeps the compose window open
      setDismissed(true);
      return;
    }
    if (e.key === 'Backspace' && text === '' && value.length > 0) {
      e.preventDefault();
      removeAt(value.length - 1);
    }
  };

  const onPaste = (e: ClipboardEvent<HTMLInputElement>) => {
    const pasted = e.clipboardData.getData('text');
    if (!pasted) return;
    const combined = `${text}${pasted}`;
    if (hasSeparator(pasted) || parseAddressList(combined).length > 1) {
      e.preventDefault();
      commitText(combined);
    }
  };

  return (
    <div className={cx('cw-field cw-recipients', focused && 'is-focused')}>
      <label htmlFor={inputId} className="cw-field-label">
        {label}
      </label>
      <div className="cw-chips">
        {value.map((a, i) => {
          const invalid = !isValidEmail(a.email);
          const unknown = !invalid && !!flagged?.has(normalizeEmail(a.email));
          const bad = invalid || unknown;
          return (
            <span
              key={`${normalizeEmail(a.email)}:${i}`}
              className={cx('cw-chip', bad && 'is-invalid')}
              title={bad ? `${displayAddress(a)} — ${invalid ? t('compose.recipients.invalid') : t('compose.recipients.unknown')}` : displayAddress(a)}
              data-invalid={bad || undefined}
              onDoubleClick={() => editAt(i)}
            >
              {bad ? (
                <Icon name="error" size={18} className="cw-chip-icon" />
              ) : (
                <Avatar name={a.name} email={a.email} size={20} decorative />
              )}
              <span className="truncate">{addressLabel(a)}</span>
              <button
                type="button"
                className="cw-chip-remove"
                aria-label={t('compose.recipients.remove', { name: addressLabel(a) })}
                disabled={disabled}
                onClick={() => removeAt(i)}
              >
                <Icon name="close" size={14} />
              </button>
            </span>
          );
        })}
        <input
          ref={inputRef}
          id={inputId}
          className="cw-recipient-input"
          type="text"
          role="combobox"
          aria-autocomplete="list"
          aria-expanded={open}
          aria-controls={listId}
          aria-activedescendant={active >= 0 ? `${listId}-${active}` : undefined}
          autoComplete="off"
          autoCapitalize="off"
          spellCheck={false}
          autoFocus={autoFocus}
          disabled={disabled}
          value={text}
          onChange={(e) => onInput(e.target.value)}
          onKeyDown={onKeyDown}
          onPaste={onPaste}
          onFocus={() => setFocused(true)}
          onBlur={() => {
            setFocused(false);
            if (textRef.current.trim()) commitText(textRef.current);
          }}
        />
      </div>
      {trailing}
      {open && (
        <div id={listId} role="listbox" aria-label={t('compose.recipients.suggestions')} className="cw-suggest">
          {suggestions.map((c, i) => (
            <div
              key={c.email}
              id={`${listId}-${i}`}
              role="option"
              tabIndex={-1}
              aria-selected={i === active}
              className={cx('cw-suggest-item', i === active && 'is-active')}
              // mousedown, not click: keeps focus (and the pending text) in the input.
              onMouseDown={(e) => {
                e.preventDefault();
                choose(c);
              }}
              onMouseEnter={() => setHighlight(i)}
            >
              <Avatar name={c.name} email={c.email} size={32} decorative />
              <span className="min-w-0 flex-1">
                <span className="block truncate text-sm text-on-surface">{c.name || c.email}</span>
                {c.name && <span className="block truncate text-xs text-on-surface-variant">{c.email}</span>}
              </span>
              {c.kind !== 'contact' && (
                <span className="cw-suggest-kind">
                  {c.kind === 'team' ? t('compose.recipients.kindTeam') : t('compose.recipients.kindAlias')}
                </span>
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  );
});
