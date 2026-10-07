/**
 * Top-bar search box [WP-E]: large rounded field (tinted, white with a shadow when focused),
 * search icon, clear button and the advanced-search popover. Enter → /mail/search?q=…;
 * on the search page it shows the current query. "/" focuses it (ui store request).
 */
import { useEffect, useRef, useState, type FormEvent } from 'react';
import { useLocation, useNavigate, useSearchParams } from 'react-router';
import { cx, IconButton } from '@/components/common';
import { t } from '@/i18n/zh';
import { useUiStore } from '@/stores/ui';
import { AdvancedSearch } from './AdvancedSearch';

export function searchPath(q: string): string {
  return `/mail/search?q=${encodeURIComponent(q.trim())}`;
}

export function SearchBox({ className }: { className?: string }) {
  const navigate = useNavigate();
  const location = useLocation();
  const [params] = useSearchParams();
  const onSearchPage = location.pathname === '/mail/search' || location.pathname.startsWith('/mail/search/');
  const currentQ = onSearchPage ? (params.get('q') ?? '') : '';
  const [value, setValue] = useState(currentQ);
  // Follow the URL's query (navigation, back / forward) without an effect.
  const [syncedQ, setSyncedQ] = useState(currentQ);
  if (syncedQ !== currentQ) {
    setSyncedQ(currentQ);
    setValue(currentQ);
  }
  const inputRef = useRef<HTMLInputElement>(null);
  const focusTick = useUiStore((s) => s.searchFocusTick);

  useEffect(() => {
    if (focusTick === 0) return;
    inputRef.current?.focus();
    inputRef.current?.select();
  }, [focusTick]);

  const go = (q: string) => {
    const v = q.trim();
    if (!v) return;
    setValue(v);
    void navigate(searchPath(v));
    inputRef.current?.blur();
  };

  const submit = (e: FormEvent) => {
    e.preventDefault();
    go(value);
  };

  return (
    <form role="search" onSubmit={submit} className={cx('w-full', className)}>
      <div
        className={cx(
          'flex h-12 w-full items-center gap-1 rounded-full bg-surface-container-high px-1 transition-[background-color,box-shadow] duration-150',
          'focus-within:bg-surface-container focus-within:shadow-elevation-1 hover:bg-[#e3ebf8] focus-within:hover:bg-surface-container',
        )}
      >
        <IconButton icon="search" label={t('mail.search.submit')} type="submit" tooltip={false} />
        <input
          ref={inputRef}
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Escape') {
              e.preventDefault();
              inputRef.current?.blur();
            }
          }}
          placeholder={t('mail.search.placeholder')}
          aria-label={t('mail.search.placeholder')}
          enterKeyHint="search"
          autoComplete="off"
          spellCheck={false}
          className="h-full min-w-0 flex-1 bg-transparent text-base text-on-surface outline-none placeholder:text-on-surface-variant focus-visible:outline-none"
        />
        {value && (
          <IconButton
            icon="close"
            label={t('mail.search.clear')}
            onClick={() => {
              setValue('');
              if (onSearchPage) void navigate('/mail/inbox');
              inputRef.current?.focus();
            }}
          />
        )}
        <AdvancedSearch trigger={<IconButton icon="tune" label={t('mail.search.options')} />} query={value} onSearch={go} />
      </div>
    </form>
  );
}
