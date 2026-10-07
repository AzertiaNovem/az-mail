/**
 * Gmail-style search query helpers [WP-E] for the search box and the advanced-search popover.
 * The server is authoritative for the grammar (docs/API.md "Search grammar"): field operators
 * `from: to: cc: bcc: subject: label: filename:`, `in:`, `is:`, `has:attachment`,
 * `after:/before:` (YYYY/MM/DD), `newer_than:/older_than:`, `larger:/smaller:` (10K, 5M),
 * `"phrases"`, `-negation`, implicit AND, `OR` between two bare terms. Values with spaces are
 * double-quoted (`from:"张 三"`); the grammar has no escapes, so `"` is dropped from values.
 *
 * - `tokenizeSearch(q)`   — lossless tokens (each keeps its raw text).
 * - `buildSearchQuery(f)` — advanced-search form → query string.
 * - `parseSearchQuery(q)` — query string → form (anything the form cannot express goes to
 *   "包含字词" verbatim), so reopening the popover shows the current search.
 */

export type SizeOp = 'larger' | 'smaller';
export type SizeUnit = 'MB' | 'KB' | 'B';
export type DateWithin = '1d' | '3d' | '1w' | '2w' | '1m' | '2m' | '6m' | '1y';

/** Days on each side of the chosen date for each "日期范围" option. */
export const DATE_WITHIN_DAYS: Record<DateWithin, number> = {
  '1d': 1,
  '3d': 3,
  '1w': 7,
  '2w': 14,
  '1m': 30,
  '2m': 60,
  '6m': 180,
  '1y': 365,
};
export const DATE_WITHIN_OPTIONS = Object.keys(DATE_WITHIN_DAYS) as DateWithin[];

/** `in:` folders the form offers. */
export const SCOPE_FOLDERS = ['inbox', 'starred', 'sent', 'drafts', 'scheduled', 'spam', 'trash', 'anywhere'] as const;
export type ScopeFolder = (typeof SCOPE_FOLDERS)[number];

/**
 * 搜索范围: '' = all mail (server default: excludes spam and trash), an `in:` folder,
 * 'is:unread' / 'is:read', or `label:<name>`.
 */
export type SearchScope = '' | ScopeFolder | 'is:unread' | 'is:read' | `label:${string}`;

export interface AdvancedSearch {
  from: string;
  to: string;
  subject: string;
  /** Free text (may contain operators the form does not model). */
  hasWords: string;
  /** Words to exclude (each becomes `-word`). */
  doesntHave: string;
  sizeOp: SizeOp;
  /** Decimal string; '' = no size filter. */
  size: string;
  sizeUnit: SizeUnit;
  dateWithin: DateWithin;
  /** 'YYYY-MM-DD' (an <input type=date> value); '' = no date filter. */
  date: string;
  scope: SearchScope;
  hasAttachment: boolean;
}

export const EMPTY_ADVANCED_SEARCH: AdvancedSearch = {
  from: '',
  to: '',
  subject: '',
  hasWords: '',
  doesntHave: '',
  sizeOp: 'larger',
  size: '',
  sizeUnit: 'MB',
  dateWithin: '1d',
  date: '',
  scope: '',
  hasAttachment: false,
};

// ───────────── tokenizer ─────────────

export interface SearchToken {
  /** Exact source text, including '-' and quotes. */
  raw: string;
  /** Operator name, lower-cased, without ':'; '' for free text. */
  field: string;
  /** Unquoted value. */
  value: string;
  negated: boolean;
  /** The value was double-quoted. */
  quoted: boolean;
  /** A bare `OR` between two terms. */
  isOr: boolean;
}

const FIELD_RE = /^([a-z_]+):/i;

/** Splits a query into tokens. Never throws; an unbalanced quote runs to the end. */
export function tokenizeSearch(q: string): SearchToken[] {
  const out: SearchToken[] = [];
  let i = 0;
  const n = q.length;
  const isSpace = (c: string | undefined) => c !== undefined && /\s/.test(c);
  while (i < n) {
    while (i < n && isSpace(q[i])) i++;
    if (i >= n) break;
    const start = i;
    let negated = false;
    if (q[i] === '-' && i + 1 < n && !isSpace(q[i + 1])) {
      negated = true;
      i++;
    }
    let field = '';
    let value = '';
    let quoted = false;
    if (q[i] === '"') {
      const end = q.indexOf('"', i + 1);
      value = end === -1 ? q.slice(i + 1) : q.slice(i + 1, end);
      i = end === -1 ? n : end + 1;
      quoted = true;
    } else {
      // Read a word; a `field:"…"` value may contain spaces inside its quotes.
      let j = i;
      while (j < n && !isSpace(q[j])) {
        if (q[j] === '"') {
          const head = q.slice(i, j);
          if (FIELD_RE.test(head) && head.endsWith(':')) {
            const end = q.indexOf('"', j + 1);
            j = end === -1 ? n : end + 1;
            quoted = true;
            break;
          }
        }
        j++;
      }
      const word = q.slice(i, j);
      i = j;
      const m = FIELD_RE.exec(word);
      if (m && word.length > m[0].length) {
        field = (m[1] ?? '').toLowerCase();
        let v = word.slice(m[0].length);
        if (quoted) v = v.replace(/^"/, '').replace(/"$/, '');
        value = v;
      } else {
        quoted = false;
        value = word;
      }
    }
    const raw = q.slice(start, i);
    out.push({ raw, field, value, negated, quoted, isOr: !negated && !quoted && field === '' && value === 'OR' });
  }
  return out;
}

// ───────────── building ─────────────

/** A value as the grammar accepts it: quoted when it contains whitespace; `"` removed. */
export function quoteValue(v: string): string {
  const s = v.replace(/"/g, '').trim();
  return /\s/.test(s) ? `"${s}"` : s;
}

function op(field: string, value: string): string {
  const v = quoteValue(value);
  return v ? `${field}:${v}` : '';
}

/** "2026-10-07" → [2026, 10, 7], or null. Accepts / as separator too. */
function parseIsoDate(s: string): [number, number, number] | null {
  const m = /^(\d{4})[-/](\d{1,2})[-/](\d{1,2})$/.exec(s.trim());
  if (!m) return null;
  const y = Number(m[1]);
  const mo = Number(m[2]);
  const d = Number(m[3]);
  const dt = new Date(Date.UTC(y, mo - 1, d));
  if (dt.getUTCFullYear() !== y || dt.getUTCMonth() !== mo - 1 || dt.getUTCDate() !== d) return null;
  return [y, mo, d];
}

const DAY_MS = 86_400_000;
const pad2 = (n: number) => String(n).padStart(2, '0');

/** Calendar arithmetic on a date: returns "YYYY/MM/DD". */
function shiftDate(ymd: [number, number, number], days: number): string {
  const dt = new Date(Date.UTC(ymd[0], ymd[1] - 1, ymd[2]) + days * DAY_MS);
  return `${dt.getUTCFullYear()}/${pad2(dt.getUTCMonth() + 1)}/${pad2(dt.getUTCDate())}`;
}

function dayNumber(ymd: [number, number, number]): number {
  return Math.round(Date.UTC(ymd[0], ymd[1] - 1, ymd[2]) / DAY_MS);
}

const UNIT_BYTES: Record<SizeUnit, number> = { MB: 1024 * 1024, KB: 1024, B: 1 };

/** `larger:` / `smaller:` value: "5M", "512K", "100"; null when not a positive number. */
export function sizeValue(size: string, unit: SizeUnit): string | null {
  const n = Number(size.trim());
  if (!size.trim() || !Number.isFinite(n) || n <= 0) return null;
  if (Number.isInteger(n)) return `${n}${unit === 'MB' ? 'M' : unit === 'KB' ? 'K' : ''}`;
  // The grammar takes integers only: express fractions in a smaller unit.
  const bytes = Math.round(n * UNIT_BYTES[unit]);
  if (bytes % 1024 === 0) return `${bytes / 1024}K`;
  return String(bytes);
}

/** Excluded words → "-a -b -\"c d\"" (quoted phrases in the input are kept together). */
function negatedTerms(text: string): string[] {
  return tokenizeSearch(text)
    .filter((tk) => !tk.isOr && (tk.value || tk.field))
    .map((tk) => {
      if (tk.field) return `-${tk.field}:${quoteValue(tk.value)}`;
      return `-${tk.quoted ? `"${tk.value.replace(/"/g, '')}"` : quoteValue(tk.value)}`;
    })
    .filter((s) => s !== '-' && s !== '-""');
}

/** Advanced-search form → query string (empty fields are skipped). */
export function buildSearchQuery(f: AdvancedSearch): string {
  const parts: string[] = [];
  parts.push(op('from', f.from), op('to', f.to), op('subject', f.subject));
  if (f.hasWords.trim()) parts.push(f.hasWords.trim());
  parts.push(...negatedTerms(f.doesntHave));
  const size = sizeValue(f.size, f.sizeUnit);
  if (size) parts.push(`${f.sizeOp}:${size}`);
  const ymd = parseIsoDate(f.date);
  if (ymd) {
    const days = DATE_WITHIN_DAYS[f.dateWithin] ?? 1;
    parts.push(`after:${shiftDate(ymd, -days)}`, `before:${shiftDate(ymd, days + 1)}`);
  }
  if (f.scope) {
    if (f.scope.startsWith('label:')) parts.push(op('label', f.scope.slice('label:'.length)));
    else if (f.scope.startsWith('is:')) parts.push(f.scope);
    else parts.push(`in:${f.scope}`);
  }
  if (f.hasAttachment) parts.push('has:attachment');
  return parts.filter(Boolean).join(' ');
}

// ───────────── parsing ─────────────

function parseSize(v: string): { size: string; unit: SizeUnit } | null {
  const m = /^(\d+)\s*([kmg]?)b?$/i.exec(v.trim());
  if (!m) return null;
  const num = Number(m[1]);
  const u = (m[2] ?? '').toLowerCase();
  if (u === 'g') return { size: String(num * 1024), unit: 'MB' };
  if (u === 'm') return { size: String(num), unit: 'MB' };
  if (u === 'k') return { size: String(num), unit: 'KB' };
  return { size: String(num), unit: 'B' };
}

const isScopeFolder = (v: string): v is ScopeFolder => (SCOPE_FOLDERS as readonly string[]).includes(v);

/** Query string → advanced-search form. Unmodelled tokens are kept verbatim in `hasWords`. */
export function parseSearchQuery(q: string): AdvancedSearch {
  const f: AdvancedSearch = { ...EMPTY_ADVANCED_SEARCH };
  const words: string[] = [];
  const excluded: string[] = [];
  let after: SearchToken | null = null;
  let before: SearchToken | null = null;

  for (const tk of tokenizeSearch(q)) {
    const field = tk.field;
    if (tk.negated) {
      if (!field) {
        excluded.push(tk.quoted ? `"${tk.value}"` : tk.value);
        continue;
      }
      words.push(tk.raw);
      continue;
    }
    switch (field) {
      case 'from':
      case 'to':
      case 'subject':
        if (!f[field]) f[field] = tk.value;
        else words.push(tk.raw);
        break;
      case 'larger':
      case 'smaller': {
        const s = parseSize(tk.value);
        if (s && !f.size) {
          f.sizeOp = field;
          f.size = s.size;
          f.sizeUnit = s.unit;
        } else words.push(tk.raw);
        break;
      }
      case 'after':
        if (!after) after = tk;
        else words.push(tk.raw);
        break;
      case 'before':
        if (!before) before = tk;
        else words.push(tk.raw);
        break;
      case 'in': {
        const v = tk.value.toLowerCase();
        if (!f.scope && isScopeFolder(v)) f.scope = v;
        else words.push(tk.raw);
        break;
      }
      case 'is': {
        const v = tk.value.toLowerCase();
        if (!f.scope && (v === 'unread' || v === 'read')) f.scope = `is:${v}`;
        else if (!f.scope && v === 'starred') f.scope = 'starred';
        else words.push(tk.raw);
        break;
      }
      case 'label':
        if (!f.scope && tk.value) f.scope = `label:${tk.value}`;
        else words.push(tk.raw);
        break;
      case 'has':
        if (tk.value.toLowerCase() === 'attachment') f.hasAttachment = true;
        else words.push(tk.raw);
        break;
      default:
        words.push(tk.raw);
    }
  }

  // after/before become "日期范围" only when they are symmetric around a date for a known span.
  if (after || before) {
    const a = after ? parseIsoDate(after.value) : null;
    const b = before ? parseIsoDate(before.value) : null;
    let matched = false;
    if (a && b) {
      const span = dayNumber(b) - dayNumber(a); // = 2N + 1
      const opt = DATE_WITHIN_OPTIONS.find((o) => 2 * DATE_WITHIN_DAYS[o] + 1 === span);
      if (opt) {
        const center = new Date((dayNumber(a) + DATE_WITHIN_DAYS[opt]) * DAY_MS);
        f.dateWithin = opt;
        f.date = `${center.getUTCFullYear()}-${pad2(center.getUTCMonth() + 1)}-${pad2(center.getUTCDate())}`;
        matched = true;
      }
    }
    if (!matched) {
      if (after) words.push(after.raw);
      if (before) words.push(before.raw);
    }
  }

  f.hasWords = words.join(' ');
  f.doesntHave = excluded.join(' ');
  return f;
}

/** Whether the form would produce an empty query. */
export function isAdvancedSearchEmpty(f: AdvancedSearch): boolean {
  return buildSearchQuery(f) === '';
}
