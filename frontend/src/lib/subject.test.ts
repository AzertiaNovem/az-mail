import { describe, expect, it } from 'vitest';
import { contrastRatio, isHexColor, labelChipColors, LABEL_COLORS, mix, parseHex, readableTextColor } from './color';
import { displaySubject, hasReplyPrefix, isEmptySubject, normalizeSubject, stripReplyPrefixes } from './subject';

describe('subject helpers', () => {
  it('detects and strips reply / forward prefixes (C7)', () => {
    expect(hasReplyPrefix('Re: 周报')).toBe(true);
    expect(hasReplyPrefix('回复：周报')).toBe(true);
    expect(hasReplyPrefix('FW: x')).toBe(true);
    expect(hasReplyPrefix('Re[2]: x')).toBe(true);
    expect(hasReplyPrefix('Re[3] x')).toBe(true);
    expect(hasReplyPrefix('Regarding 周报')).toBe(false);
    expect(hasReplyPrefix('周报')).toBe(false);
    expect(stripReplyPrefixes('Re: 回复：Fwd: 轉寄: 答复: 周报 ')).toBe('周报');
    expect(stripReplyPrefixes('RE:RE:  hello')).toBe('hello');
    expect(normalizeSubject('Re:  Weekly   Report')).toBe('weekly report');
  });

  it('displays empty subjects as (无主题)', () => {
    expect(displaySubject('  ')).toBe('(无主题)');
    expect(displaySubject(null)).toBe('(无主题)');
    expect(displaySubject(' a\n b ')).toBe('a b');
    expect(isEmptySubject('')).toBe(true);
    expect(isEmptySubject('x')).toBe(false);
  });
});

describe('label colors', () => {
  it('parses and mixes hex colors', () => {
    expect(isHexColor('#abc')).toBe(true);
    expect(isHexColor('#a1b2c3')).toBe(true);
    expect(isHexColor('red')).toBe(false);
    expect(parseHex('#fff')).toEqual({ r: 255, g: 255, b: 255 });
    expect(parseHex('nope')).toEqual(parseHex('#9aa0a6'));
    expect(mix('#000000', '#ffffff', 0.5)).toBe('#808080');
  });

  it('chip colors stay readable (WCAG AA) for every palette color', () => {
    for (const c of [...LABEL_COLORS, '#ffff00', '#00ffff', 'bogus']) {
      const { background, foreground } = labelChipColors(c);
      expect(contrastRatio(foreground, background)).toBeGreaterThanOrEqual(4.5);
    }
  });

  it('picks black or white text', () => {
    expect(readableTextColor('#ffffff')).toBe('#000000');
    expect(readableTextColor('#0b57d0')).toBe('#ffffff');
  });
});
