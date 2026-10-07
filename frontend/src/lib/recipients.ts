/**
 * Recipient helpers [WP-F]: address parsing / validation / formatting, and the reply,
 * reply-all and forward rules (DESIGN.md §5 "Compose store" behaviour, §1 C3).
 *
 * Rules
 * - reply      → `reply_to[0]`, else `from`. Replying to a message *from me* (sent copy,
 *                shared alias copy, self-sent note) continues with its original To.
 * - reply_all  → reply target + original To in To, original Cc in Cc, minus my identities,
 *                deduplicated (Gmail behaviour; Bcc is never copied).
 * - forward    → no recipients.
 * - default From: the identity the parent was delivered to (`delivered_to`, alias fan-out)
 *   if the user may send as it, else the parent's From for my own messages, else the first of
 *   my identities found in the parent's To/Cc, else the default identity.
 *
 * Identity matching is case-insensitive and ignores a `+tag` in the local part (the server
 * routes `alice+x@` to `alice@`, DESIGN §1 C1). Deduplication only folds case.
 */
import type { Address, Identity, Message } from '@/api/types';

/** Server limit per To / Cc / Bcc field (DESIGN §1 C12). */
export const MAX_RECIPIENTS_PER_FIELD = 50;

// ───────────── validation / normalization ─────────────

const LOCAL_RE = /^[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+(\.[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+)*$/;
const LABEL_RE = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;
const TLD_RE = /^(?:[A-Za-z]{2,63}|xn--[A-Za-z0-9-]{1,59})$/;

/** Pragmatic RFC 5321 check: dot-atom local part, LDH domain labels, alphabetic (or IDNA) TLD. */
export function isValidEmail(email: string): boolean {
  if (!email || email.length > 254) return false;
  const at = email.lastIndexOf('@');
  if (at < 1 || at === email.length - 1) return false;
  const local = email.slice(0, at);
  const domain = email.slice(at + 1);
  if (local.length > 64 || !LOCAL_RE.test(local)) return false;
  const labels = domain.split('.');
  if (labels.length < 2) return false;
  if (!labels.every((l) => LABEL_RE.test(l))) return false;
  return TLD_RE.test(labels[labels.length - 1] ?? '');
}

/** Trimmed, lower-cased address (dedupe key). */
export function normalizeEmail(email: string): string {
  return email.trim().toLowerCase();
}

/** Mailbox identity key: lower-cased with any `+tag` removed from the local part. */
export function identityKey(email: string): string {
  const e = normalizeEmail(email);
  const at = e.lastIndexOf('@');
  if (at < 1) return e;
  const plus = e.indexOf('+');
  return plus > 0 && plus < at ? e.slice(0, plus) + e.slice(at) : e;
}

export function sameEmail(a: string, b: string): boolean {
  return normalizeEmail(a) === normalizeEmail(b);
}

// ───────────── formatting ─────────────

/** Characters that force a display name into a quoted-string (RFC 5322 specials). */
const SPECIALS_RE = /[()<>[\]:;@\\,."]/;

/** `"张三 (运营)" <a@b>`; a bare address when there is no name. */
export function formatAddress(a: Address): string {
  const name = a.name.trim();
  if (!name || sameEmail(name, a.email)) return a.email;
  const display = SPECIALS_RE.test(name) ? `"${name.replace(/(["\\])/g, '\\$1')}"` : name;
  return `${display} <${a.email}>`;
}

/** Human form without RFC quoting, e.g. `张三 <a@b>` (quote headers, tooltips). */
export function displayAddress(a: Address): string {
  const name = a.name.trim();
  return name && !sameEmail(name, a.email) ? `${name} <${a.email}>` : a.email;
}

/** Chip text: the name, or the address when there is none. */
export function addressLabel(a: Address): string {
  return a.name.trim() || a.email;
}

// ───────────── parsing ─────────────

/** ASCII and full-width list separators (people paste lists from WeChat / spreadsheets). */
const SEPARATORS = new Set([',', ';', '，', '；', '、', '\n', '\r', '\t']);

/** Whether typed text contains a list separator (commit the chips before it). */
export function hasSeparator(text: string): boolean {
  for (const ch of text) if (SEPARATORS.has(ch)) return true;
  return false;
}

/** Splits on separators outside quotes, angle brackets and comments. */
function splitTokens(input: string): string[] {
  const out: string[] = [];
  let cur = '';
  let inQuote = false;
  let inAngle = false;
  let paren = 0;
  for (let i = 0; i < input.length; i++) {
    const ch = input[i] ?? '';
    if (inQuote) {
      cur += ch;
      if (ch === '\\' && i + 1 < input.length) cur += input[++i] ?? '';
      else if (ch === '"') inQuote = false;
      continue;
    }
    if (ch === '"') inQuote = true;
    else if (ch === '<') inAngle = true;
    else if (ch === '>') inAngle = false;
    else if (ch === '(') paren++;
    else if (ch === ')' && paren > 0) paren--;
    else if (!inAngle && paren === 0 && SEPARATORS.has(ch)) {
      out.push(cur);
      cur = '';
      continue;
    }
    cur += ch;
  }
  out.push(cur);
  return out.map((s) => s.trim()).filter(Boolean);
}

function cleanEmail(raw: string): string {
  let e = raw.trim().replace(/^mailto:/i, '');
  if (e.startsWith('"') && e.endsWith('"') && e.length > 1) e = e.slice(1, -1);
  return e.replace(/^<|>$/g, '').trim();
}

function cleanName(raw: string): string {
  let n = raw.trim();
  if (n.length > 1 && n.startsWith('"') && n.endsWith('"')) n = n.slice(1, -1).replace(/\\(.)/g, '$1');
  else if (n.length > 1 && n.startsWith("'") && n.endsWith("'")) n = n.slice(1, -1);
  return n.replace(/\s+/g, ' ').trim();
}

function parseToken(token: string): Address[] {
  const angle = /^(.*?)<([^<>]*)>(.*)$/s.exec(token);
  if (angle) {
    const email = cleanEmail(angle[2] ?? '');
    let name = cleanName(`${angle[1] ?? ''} ${angle[3] ?? ''}`);
    if (sameEmail(name, email)) name = '';
    return [{ name, email }];
  }
  // Legacy "a@b (Name)" form.
  const comment = /^(\S+@\S+)\s*\((.*)\)$/s.exec(token);
  if (comment) return [{ name: cleanName(comment[2] ?? ''), email: cleanEmail(comment[1] ?? '') }];

  const words = token.split(/\s+/);
  if (words.length > 1) {
    // "a@b c@d" pasted with spaces only → several addresses.
    if (words.every((w) => isValidEmail(cleanEmail(w)))) return words.map((w) => ({ name: '', email: cleanEmail(w) }));
    // "张三 zs@example.com" → name + address.
    const last = cleanEmail(words[words.length - 1] ?? '');
    if (isValidEmail(last)) return [{ name: cleanName(words.slice(0, -1).join(' ')), email: last }];
  }
  return [{ name: '', email: cleanEmail(token) }];
}

/**
 * Parses free text such as `a@b, 张三 <c@d>; "Lee, Ann" <e@f>` into addresses. Tokens that are
 * not valid addresses are kept (as `{name:'', email: token}`) so the UI can show them as
 * invalid chips; check them with `isValidEmail`.
 */
export function parseAddressList(input: string): Address[] {
  return splitTokens(input).flatMap(parseToken).filter((a) => a.email !== '');
}

/** Removes duplicates (case-insensitive), keeping the first position and the first non-empty name. */
export function dedupeAddresses(list: readonly Address[]): Address[] {
  const out: Address[] = [];
  const index = new Map<string, number>();
  for (const a of list) {
    const key = normalizeEmail(a.email);
    const at = index.get(key);
    if (at === undefined) {
      index.set(key, out.length);
      out.push({ name: a.name, email: a.email.trim() });
    } else if (!out[at]!.name.trim() && a.name.trim()) {
      out[at] = { ...out[at]!, name: a.name };
    }
  }
  return out;
}

/** `existing` followed by the new addresses that are not already in it. */
export function mergeAddresses(existing: readonly Address[], added: readonly Address[]): Address[] {
  return dedupeAddresses([...existing, ...added]);
}

export function containsEmail(list: readonly Address[], email: string): boolean {
  const key = normalizeEmail(email);
  return list.some((a) => normalizeEmail(a.email) === key);
}

// ───────────── identities / reply rules ─────────────

export type IsMine = (email: string) => boolean;

/** Predicate over my mailbox + every identity I may send as (+tags ignored). */
export function mineMatcher(identities: readonly Identity[], extraEmails: readonly string[] = []): IsMine {
  const keys = new Set([...identities.map((i) => identityKey(i.email)), ...extraEmails.map(identityKey)]);
  return (email) => keys.has(identityKey(email));
}

export type ReplyMode = 'reply' | 'reply_all';

export interface ReplyRecipients {
  to: Address[];
  cc: Address[];
}

function copyList(list: readonly Address[]): Address[] {
  return dedupeAddresses(list.filter((a) => a.email.trim() !== ''));
}

/** Recipients of a reply / reply-all to `parent` (see the module comment for the rules). */
export function replyRecipients(parent: Message, mode: ReplyMode, isMine: IsMine): ReplyRecipients {
  const fromMe = parent.direction === 'out' || isMine(parent.from.email);
  const notMine = (a: Address) => !isMine(a.email);

  if (fromMe) {
    const originalTo = copyList(parent.to);
    const othersTo = originalTo.filter(notMine);
    if (mode === 'reply') {
      if (othersTo.length) return { to: othersTo, cc: [] };
      return { to: originalTo.length ? originalTo : [parent.from], cc: [] };
    }
    let to = othersTo;
    let cc = copyList(parent.cc).filter((a) => notMine(a) && !containsEmail(to, a.email));
    if (!to.length && cc.length) {
      to = cc;
      cc = [];
    }
    if (!to.length) to = originalTo.length ? originalTo : [parent.from]; // a note to myself
    return { to, cc };
  }

  const target = parent.reply_to[0] ?? parent.from;
  if (mode === 'reply') return { to: [{ name: target.name, email: target.email }], cc: [] };
  const to = dedupeAddresses([target, ...parent.to.filter(notMine)]);
  const cc = copyList(parent.cc).filter((a) => notMine(a) && !containsEmail(to, a.email));
  return { to, cc };
}

/** The From identity a new message / reply / forward starts with (module comment). */
export function defaultFromIdentity(identities: readonly Identity[], parent?: Message | null): Identity | undefined {
  if (identities.length === 0) return undefined;
  const find = (email: string | null | undefined) =>
    email ? identities.find((i) => identityKey(i.email) === identityKey(email)) : undefined;
  if (parent) {
    const delivered = find(parent.delivered_to);
    if (delivered) return delivered;
    if (parent.direction === 'out') {
      const own = find(parent.from.email);
      if (own) return own;
    }
    for (const a of [...parent.to, ...parent.cc]) {
      const hit = find(a.email);
      if (hit) return hit;
    }
  }
  return identities.find((i) => i.is_default) ?? identities.find((i) => i.kind === 'user') ?? identities[0];
}

// ───────────── subjects ─────────────

const REPLY_PREFIX_RE = /^\s*(re|回复|答复|回覆)\s*(\[\d+\])?\s*[:：]/i;
const FORWARD_PREFIX_RE = /^\s*(fwd?|转发|轉寄|转寄)\s*(\[\d+\])?\s*[:：]/i;

/** "Re: …" unless the subject already carries a reply prefix. */
export function replySubject(subject: string): string {
  const s = subject.trim();
  return REPLY_PREFIX_RE.test(s) ? s : `Re: ${s}`.trimEnd();
}

/** "Fwd: …" unless the subject already carries a forward prefix. */
export function forwardSubject(subject: string): string {
  const s = subject.trim();
  return FORWARD_PREFIX_RE.test(s) ? s : `Fwd: ${s}`.trimEnd();
}
