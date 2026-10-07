import { describe, expect, it } from 'vitest';
import {
  buildSearchQuery,
  EMPTY_ADVANCED_SEARCH,
  isAdvancedSearchEmpty,
  parseSearchQuery,
  quoteValue,
  sizeValue,
  tokenizeSearch,
  type AdvancedSearch,
} from './searchQuery';

const form = (over: Partial<AdvancedSearch>): AdvancedSearch => ({ ...EMPTY_ADVANCED_SEARCH, ...over });

describe('tokenizeSearch', () => {
  it('splits operators, phrases, negations and OR losslessly', () => {
    const q = 'from:"张 三" subject:周报 -草稿 "季度 总结" a OR b -from:bob@x.test has:attachment';
    const tokens = tokenizeSearch(q);
    expect(tokens.map((t) => t.raw)).toEqual([
      'from:"张 三"',
      'subject:周报',
      '-草稿',
      '"季度 总结"',
      'a',
      'OR',
      'b',
      '-from:bob@x.test',
      'has:attachment',
    ]);
    expect(tokens[0]).toMatchObject({ field: 'from', value: '张 三', quoted: true, negated: false });
    expect(tokens[2]).toMatchObject({ field: '', value: '草稿', negated: true });
    expect(tokens[3]).toMatchObject({ field: '', value: '季度 总结', quoted: true });
    expect(tokens[5]!.isOr).toBe(true);
    expect(tokens[7]).toMatchObject({ field: 'from', value: 'bob@x.test', negated: true });
  });

  it('tolerates unbalanced quotes, lone dashes, empty operators and extra spaces', () => {
    expect(tokenizeSearch('  "open phrase').map((t) => t.value)).toEqual(['open phrase']);
    expect(tokenizeSearch('a - b').map((t) => t.raw)).toEqual(['a', '-', 'b']);
    expect(tokenizeSearch('from: bob')[0]).toMatchObject({ field: '', value: 'from:' });
    expect(tokenizeSearch('from:"x y').map((t) => [t.field, t.value])).toEqual([['from', 'x y']]);
    expect(tokenizeSearch('')).toEqual([]);
    expect(tokenizeSearch('https://example.com/a')[0]).toMatchObject({ field: 'https', value: '//example.com/a' });
  });
});

describe('buildSearchQuery', () => {
  it('builds every field in grammar order', () => {
    const q = buildSearchQuery(
      form({
        from: 'alice@team.test',
        to: '张 三',
        subject: '周报',
        hasWords: 'deploy',
        doesntHave: '测试 "dry run"',
        size: '5',
        sizeUnit: 'MB',
        sizeOp: 'larger',
        date: '2026-10-07',
        dateWithin: '1w',
        scope: 'sent',
        hasAttachment: true,
      }),
    );
    expect(q).toBe(
      'from:alice@team.test to:"张 三" subject:周报 deploy -测试 -"dry run" larger:5M after:2026/09/30 before:2026/10/15 in:sent has:attachment',
    );
  });

  it('handles scopes, sizes and empty forms', () => {
    expect(buildSearchQuery(form({ scope: 'label:项目 A' }))).toBe('label:"项目 A"');
    expect(buildSearchQuery(form({ scope: 'is:unread' }))).toBe('is:unread');
    expect(buildSearchQuery(form({ scope: 'anywhere', hasWords: 'x' }))).toBe('x in:anywhere');
    expect(buildSearchQuery(form({ size: '300', sizeUnit: 'KB', sizeOp: 'smaller' }))).toBe('smaller:300K');
    expect(buildSearchQuery(form({ size: '1.5', sizeUnit: 'MB' }))).toBe('larger:1536K');
    expect(buildSearchQuery(form({ size: 'abc' }))).toBe('');
    expect(buildSearchQuery(form({ date: '2026-02-30' }))).toBe('');
    expect(isAdvancedSearchEmpty(EMPTY_ADVANCED_SEARCH)).toBe(true);
    expect(isAdvancedSearchEmpty(form({ hasAttachment: true }))).toBe(false);
  });

  it('crosses month and year boundaries in date ranges', () => {
    expect(buildSearchQuery(form({ date: '2026-01-01', dateWithin: '1d' }))).toBe('after:2025/12/31 before:2026/01/03');
    expect(buildSearchQuery(form({ date: '2024-02-28', dateWithin: '1d' }))).toBe('after:2024/02/27 before:2024/03/01');
  });

  it('quotes values with spaces and drops double quotes', () => {
    expect(quoteValue('a b')).toBe('"a b"');
    expect(quoteValue('say "hi"')).toBe('"say hi"');
    expect(quoteValue('  plain ')).toBe('plain');
    expect(sizeValue('10', 'B')).toBe('10');
    expect(sizeValue('0', 'MB')).toBeNull();
    expect(sizeValue('0.5', 'KB')).toBe('512');
  });
});

describe('parseSearchQuery', () => {
  it('round-trips the advanced form', () => {
    const f = form({
      from: 'alice@team.test',
      to: '张 三',
      subject: '周报',
      hasWords: 'deploy',
      doesntHave: '测试 "dry run"',
      size: '5',
      sizeUnit: 'MB',
      date: '2026-10-07',
      dateWithin: '1w',
      scope: 'sent',
      hasAttachment: true,
    });
    expect(parseSearchQuery(buildSearchQuery(f))).toEqual(f);
  });

  it('keeps what the form cannot express in 包含字词', () => {
    const f = parseSearchQuery('from:a from:b after:2026/01/01 is:starred newer_than:3d -from:spam@x.test cc:c OR d');
    expect(f.from).toBe('a');
    expect(f.scope).toBe('starred');
    // Input order is kept; an unpaired after:/before: is appended.
    expect(f.hasWords).toBe('from:b newer_than:3d -from:spam@x.test cc:c OR d after:2026/01/01');
    expect(f.date).toBe('');
  });

  it('maps label / is: scopes and sizes', () => {
    expect(parseSearchQuery('label:"项目 A"').scope).toBe('label:项目 A');
    expect(parseSearchQuery('is:read').scope).toBe('is:read');
    expect(parseSearchQuery('in:bogus').hasWords).toBe('in:bogus');
    expect(parseSearchQuery('smaller:2G')).toMatchObject({ sizeOp: 'smaller', size: '2048', sizeUnit: 'MB' });
    expect(parseSearchQuery('larger:100')).toMatchObject({ size: '100', sizeUnit: 'B' });
    expect(parseSearchQuery('larger:huge').hasWords).toBe('larger:huge');
  });

  it('keeps asymmetric date ranges as text', () => {
    const f = parseSearchQuery('after:2026/10/01 before:2026/10/05');
    expect(f.date).toBe('');
    expect(f.hasWords).toBe('after:2026/10/01 before:2026/10/05');
    expect(parseSearchQuery('after:2026/09/30 before:2026/10/15')).toMatchObject({ date: '2026-10-07', dateWithin: '1w' });
  });
});
