import { describe, expect, it } from 'vitest';
import { makeMe } from '@/components/compose/testFixtures';
import { timezoneOptions } from './timezones';
import { diffGeneral, generalFormFrom, normalizeGeneral, validateGeneral, validateLabelName, validatePasswordChange } from './validation';

const base = generalFormFrom(makeMe().settings);

describe('general settings validation', () => {
  it('accepts the defaults', () => {
    expect(validateGeneral(base)).toEqual({});
  });

  it('requires a display name of at most 100 characters (CJK counted as one)', () => {
    expect(validateGeneral({ ...base, display_name: '   ' }).display_name).toBe('请输入显示名称');
    expect(validateGeneral({ ...base, display_name: '张'.repeat(100) }).display_name).toBeUndefined();
    expect(validateGeneral({ ...base, display_name: '张'.repeat(101) }).display_name).toBe('显示名称不能超过 100 个字符');
  });

  it('checks undo seconds, page size and time zone', () => {
    expect(validateGeneral({ ...base, undo_send_seconds: 7 as never }).undo_send_seconds).toBe('请选择有效的撤销时间');
    expect(validateGeneral({ ...base, page_size: 9 }).page_size).toBe('每页显示数量须在 10 到 100 之间');
    expect(validateGeneral({ ...base, page_size: 101 }).page_size).toBeDefined();
    expect(validateGeneral({ ...base, page_size: 100 }).page_size).toBeUndefined();
    expect(validateGeneral({ ...base, timezone: 'Nowhere/City' }).timezone).toBe('请选择有效的时区');
  });

  it('limits the signature to 64 KiB of UTF-8', () => {
    expect(validateGeneral({ ...base, signature_html: `<p>${'签'.repeat(22_000)}</p>` }).signature_html).toBe('签名过长（最多 64 KB）');
    expect(validateGeneral({ ...base, signature_html: `<p>${'签'.repeat(20_000)}</p>` }).signature_html).toBeUndefined();
  });

  it('diffs only changed (normalized) fields', () => {
    expect(diffGeneral(base, base)).toEqual({});
    expect(diffGeneral(base, { ...base, display_name: ' 张三 ' })).toEqual({}); // trimmed equal
    expect(diffGeneral(base, { ...base, page_size: 25, remote_images: 'always' })).toEqual({ page_size: 25, remote_images: 'always' });
    expect(diffGeneral({ ...base, signature_html: '' }, { ...base, signature_html: '<p></p>' })).toEqual({}); // blank editor = ''
    expect(diffGeneral({ ...base, trusted_image_senders: ['a@x', 'b@x'] }, { ...base, trusted_image_senders: ['a@x'] })).toEqual({
      trusted_image_senders: ['a@x'],
    });
    expect(normalizeGeneral({ ...base, signature_html: '<p><br></p>' }).signature_html).toBe('');
  });
});

describe('password change validation', () => {
  it('requires all fields, ≥ 8 characters, a different and confirmed new password', () => {
    expect(validatePasswordChange({ current: '', next: '', confirm: '' })).toEqual({ current: '请输入当前密码', next: '请输入新密码' });
    expect(validatePasswordChange({ current: 'old-pass', next: 'short', confirm: 'short' })).toEqual({ next: '新密码至少需要 8 个字符' });
    expect(validatePasswordChange({ current: 'same-pass', next: 'same-pass', confirm: 'same-pass' })).toEqual({ next: '新密码不能与当前密码相同' });
    expect(validatePasswordChange({ current: 'old-pass', next: 'new-pass-1', confirm: 'new-pass-2' })).toEqual({ confirm: '两次输入的新密码不一致' });
    expect(validatePasswordChange({ current: 'old-pass', next: 'new-pass-1', confirm: 'new-pass-1' })).toEqual({});
  });
});

describe('labels and time zones', () => {
  it('validates label names', () => {
    expect(validateLabelName('  ')).toBe('请输入标签名称');
    // The server limit: 64 code points (repo/accounts.cpp kMaxLabelName), counted as code points.
    expect(validateLabelName('x'.repeat(65))).toBe('标签名称不能超过 64 个字符');
    expect(validateLabelName('x'.repeat(64))).toBeNull();
    expect(validateLabelName('😀'.repeat(64))).toBeNull(); // 128 UTF-16 units, 64 characters
    expect(validateLabelName('😀'.repeat(65))).toBe('标签名称不能超过 64 个字符');
    expect(validateLabelName('财务/报销')).toBeNull();
  });

  it('lists common zones sorted by offset, with the current one included', () => {
    const now = Date.UTC(2026, 9, 7);
    const opts = timezoneOptions('Asia/Shanghai', now);
    expect(opts.find((o) => o.value === 'Asia/Shanghai')?.label).toBe('(GMT+08:00) 北京、上海');
    const la = opts.findIndex((o) => o.value === 'America/Los_Angeles');
    const sh = opts.findIndex((o) => o.value === 'Asia/Shanghai');
    expect(la).toBeLessThan(sh);
    expect(timezoneOptions('Africa/Nairobi', now).some((o) => o.value === 'Africa/Nairobi')).toBe(true);
    expect(timezoneOptions('Bogus/Zone', now).some((o) => o.value === 'Bogus/Zone')).toBe(false);
  });
});
