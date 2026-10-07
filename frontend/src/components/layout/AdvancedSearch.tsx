/**
 * Advanced search popover [WP-E] (Gmail "显示搜索选项"): 发件人 / 收件人 / 主题 / 包含字词 /
 * 不包含 / 大小 / 日期范围 / 搜索范围 / 包含附件. Opens pre-filled from the current query
 * (parseSearchQuery) and builds the query string on 搜索 (buildSearchQuery).
 */
import { useId, useState, type FormEvent, type ReactElement, type ReactNode } from 'react';
import { Button, Checkbox, cx, Popover } from '@/components/common';
import { useLabels } from '@/components/mail/queries';
import { t, type MessageKey } from '@/i18n/zh';
import {
  buildSearchQuery,
  DATE_WITHIN_OPTIONS,
  EMPTY_ADVANCED_SEARCH,
  isAdvancedSearchEmpty,
  parseSearchQuery,
  SCOPE_FOLDERS,
  type AdvancedSearch as Form,
  type SearchScope,
  type SizeOp,
  type SizeUnit,
} from '@/lib/searchQuery';

export interface AdvancedSearchProps {
  trigger: ReactElement;
  /** Current query (pre-fills the form when opened). */
  query: string;
  onSearch: (q: string) => void;
}

const inputClass =
  'h-8 min-w-0 flex-1 border-0 border-b border-outline-variant bg-transparent px-0 text-sm text-on-surface outline-none ' +
  'transition-colors hover:border-outline focus:border-b-2 focus:border-primary focus-visible:outline-none';
const selectClass =
  'h-8 cursor-pointer rounded border border-outline-variant bg-surface-container px-2 text-sm text-on-surface outline-none ' +
  'hover:border-outline focus:border-primary focus-visible:outline-none';

function Row({ label, htmlFor, children }: { label: string; htmlFor?: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[96px_minmax(0,1fr)] items-center gap-3 sm:grid-cols-[120px_minmax(0,1fr)]">
      <label htmlFor={htmlFor} className="text-sm text-on-surface-variant">
        {label}
      </label>
      <div className="flex min-w-0 items-center gap-2">{children}</div>
    </div>
  );
}

export function AdvancedSearch({ trigger, query, onSearch }: AdvancedSearchProps) {
  const [open, setOpen] = useState(false);
  const [form, setForm] = useState<Form>(EMPTY_ADVANCED_SEARCH);
  const labels = useLabels().data ?? [];
  const id = useId();
  const set = <K extends keyof Form>(k: K, v: Form[K]) => setForm((f) => ({ ...f, [k]: v }));

  const submit = (e: FormEvent) => {
    e.preventDefault();
    // React events bubble through portals: keep this submit away from the search box's form.
    e.stopPropagation();
    const q = buildSearchQuery(form);
    if (!q) return;
    setOpen(false);
    onSearch(q);
  };

  return (
    <Popover
      open={open}
      onOpenChange={(o) => {
        if (o) setForm(parseSearchQuery(query));
        setOpen(o);
      }}
      trigger={trigger}
      align="end"
      sideOffset={10}
      className="w-[min(640px,calc(100vw-24px))] p-5 sm:p-6"
      aria-label={t('mail.advanced.title')}
    >
      <form onSubmit={submit} className="flex flex-col gap-3.5">
        <Row label={t('mail.advanced.from')} htmlFor={`${id}-from`}>
          <input id={`${id}-from`} className={inputClass} value={form.from} onChange={(e) => set('from', e.target.value)} autoFocus />
        </Row>
        <Row label={t('mail.advanced.to')} htmlFor={`${id}-to`}>
          <input id={`${id}-to`} className={inputClass} value={form.to} onChange={(e) => set('to', e.target.value)} />
        </Row>
        <Row label={t('mail.advanced.subject')} htmlFor={`${id}-subject`}>
          <input id={`${id}-subject`} className={inputClass} value={form.subject} onChange={(e) => set('subject', e.target.value)} />
        </Row>
        <Row label={t('mail.advanced.hasWords')} htmlFor={`${id}-has`}>
          <input id={`${id}-has`} className={inputClass} value={form.hasWords} onChange={(e) => set('hasWords', e.target.value)} />
        </Row>
        <Row label={t('mail.advanced.doesntHave')} htmlFor={`${id}-not`}>
          <input id={`${id}-not`} className={inputClass} value={form.doesntHave} onChange={(e) => set('doesntHave', e.target.value)} />
        </Row>
        <Row label={t('mail.advanced.size')} htmlFor={`${id}-size`}>
          <select
            className={selectClass}
            value={form.sizeOp}
            onChange={(e) => set('sizeOp', e.target.value as SizeOp)}
            aria-label={t('mail.advanced.size')}
          >
            <option value="larger">{t('mail.advanced.sizeLarger')}</option>
            <option value="smaller">{t('mail.advanced.sizeSmaller')}</option>
          </select>
          <input
            id={`${id}-size`}
            className={cx(inputClass, 'max-w-[120px]')}
            inputMode="decimal"
            value={form.size}
            onChange={(e) => set('size', e.target.value.replace(/[^\d.]/g, ''))}
            aria-label={t('mail.advanced.sizeValue')}
          />
          <select
            className={selectClass}
            value={form.sizeUnit}
            onChange={(e) => set('sizeUnit', e.target.value as SizeUnit)}
            aria-label={t('mail.advanced.sizeUnit')}
          >
            <option value="MB">{t('mail.advanced.unitMB')}</option>
            <option value="KB">{t('mail.advanced.unitKB')}</option>
            <option value="B">{t('mail.advanced.unitB')}</option>
          </select>
        </Row>
        <Row label={t('mail.advanced.dateWithin')} htmlFor={`${id}-date`}>
          <select
            className={selectClass}
            value={form.dateWithin}
            onChange={(e) => set('dateWithin', e.target.value as Form['dateWithin'])}
            aria-label={t('mail.advanced.dateWithin')}
          >
            {DATE_WITHIN_OPTIONS.map((o) => (
              <option key={o} value={o}>
                {t(`mail.advanced.within.${o}` as MessageKey)}
              </option>
            ))}
          </select>
          <span className="shrink-0 text-sm text-on-surface-variant">{t('mail.advanced.dateWithinSuffix')}</span>
          <input
            id={`${id}-date`}
            type="date"
            className={cx(inputClass, 'max-w-[170px]')}
            value={form.date}
            onChange={(e) => set('date', e.target.value)}
            aria-label={t('mail.advanced.date')}
          />
        </Row>
        <Row label={t('mail.advanced.scope')} htmlFor={`${id}-scope`}>
          <select
            id={`${id}-scope`}
            className={cx(selectClass, 'min-w-0 flex-1')}
            value={form.scope}
            onChange={(e) => set('scope', e.target.value as SearchScope)}
          >
            <option value="">{t('mail.advanced.scopes.all')}</option>
            {SCOPE_FOLDERS.map((f) => (
              <option key={f} value={f}>
                {t(`mail.advanced.scopes.${f}` as MessageKey)}
              </option>
            ))}
            <option value="is:unread">{t('mail.advanced.scopes.unread')}</option>
            <option value="is:read">{t('mail.advanced.scopes.read')}</option>
            {labels.map((l) => (
              <option key={l.id} value={`label:${l.name}`}>
                {t('mail.advanced.scopes.label', { name: l.name })}
              </option>
            ))}
            {form.scope.startsWith('label:') && !labels.some((l) => `label:${l.name}` === form.scope) && (
              <option value={form.scope}>{t('mail.advanced.scopes.label', { name: form.scope.slice(6) })}</option>
            )}
          </select>
        </Row>
        <div className="pl-[108px] sm:pl-[132px]">
          <Checkbox checked={form.hasAttachment} onCheckedChange={(c) => set('hasAttachment', c)} label={t('mail.advanced.hasAttachment')} />
        </div>
        <div className="mt-2 flex justify-end gap-2">
          <Button variant="text" onClick={() => setForm(EMPTY_ADVANCED_SEARCH)}>
            {t('mail.advanced.reset')}
          </Button>
          <Button type="submit" disabled={isAdvancedSearchEmpty(form)}>
            {t('mail.advanced.search')}
          </Button>
        </div>
      </form>
    </Popover>
  );
}
