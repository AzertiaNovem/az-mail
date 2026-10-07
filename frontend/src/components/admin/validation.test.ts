import { describe, expect, it } from 'vitest';
import { aliasInput } from './AliasDialog';
import { formatDateTime } from './format';
import { userPatch } from './UserDialogs';
import { splitEmail, validateAlias, validateDomainName, validateLocalPart, validateUserCreate, validateUserEdit } from './validation';

describe('admin validation', () => {
  it('validates mailbox local parts', () => {
    expect(validateLocalPart('zhangsan')).toBeNull();
    expect(validateLocalPart('zhang.san+ops_1-a')).toBeNull();
    expect(validateLocalPart('')).toBe('请输入邮箱地址');
    expect(validateLocalPart('.zs')).toBe('只能包含字母、数字和 . _ + -，且不能以 . 开头或结尾');
    expect(validateLocalPart('zs.')).not.toBeNull();
    expect(validateLocalPart('z..s')).not.toBeNull();
    expect(validateLocalPart('张三')).not.toBeNull();
    expect(validateLocalPart('a@b')).not.toBeNull();
    expect(validateLocalPart('x'.repeat(65))).toBe('邮箱地址不能超过 64 个字符');
  });

  it('validates domain names', () => {
    expect(validateDomainName('example.com')).toBeNull();
    expect(validateDomainName('Mail.Example.CN')).toBeNull();
    expect(validateDomainName('xn--fiqs8s.xn--fiqz9s')).toBeNull();
    for (const bad of ['', 'localhost', 'a..b.com', '-a.com', 'a.c', 'a b.com', 'http://a.com'])
      expect(validateDomainName(bad)).toBe('请输入有效的域名，例如 example.com');
  });

  it('validates the create-user dialog', () => {
    expect(validateUserCreate({ localPart: '', domain: '', displayName: '', password: '', isAdmin: false })).toEqual({
      localPart: '请输入邮箱地址',
      domain: '请选择域名',
      displayName: '请输入显示名称',
      password: '请输入初始密码',
    });
    expect(validateUserCreate({ localPart: 'zs', domain: 'az.test', displayName: '张三', password: '1234567', isAdmin: true })).toEqual({
      password: '密码至少需要 8 个字符',
    });
    expect(validateUserCreate({ localPart: 'zs', domain: 'az.test', displayName: '张'.repeat(101), password: '12345678', isAdmin: false })).toEqual({
      displayName: '显示名称不能超过 100 个字符',
    });
    expect(validateUserCreate({ localPart: 'zs', domain: 'az.test', displayName: '张三', password: '12345678', isAdmin: false })).toEqual({});
  });

  it('validates the edit-user dialog (password optional) and builds minimal patches', () => {
    expect(validateUserEdit({ displayName: '张三', isAdmin: false, disabled: false, password: '' })).toEqual({});
    expect(validateUserEdit({ displayName: '', isAdmin: false, disabled: false, password: 'short' })).toEqual({
      displayName: '请输入显示名称',
      password: '密码至少需要 8 个字符',
    });
    const user = {
      id: 2,
      email: 'zs@az.test',
      display_name: '张三',
      is_admin: false,
      disabled: false,
      created_at: 0,
      last_login_at: null,
      message_count: 0,
      storage_bytes: 0,
      aliases: [],
    };
    expect(userPatch(user, { displayName: ' 张三 ', isAdmin: false, disabled: false, password: '' })).toEqual({});
    expect(userPatch(user, { displayName: '张三丰', isAdmin: true, disabled: true, password: 'new-password' })).toEqual({
      display_name: '张三丰',
      is_admin: true,
      disabled: true,
      password: 'new-password',
    });
  });

  it('validates aliases and builds the request', () => {
    expect(validateAlias({ localPart: 'support', domain: 'az.test', displayName: '', shareSent: true, members: [] })).toEqual({});
    expect(validateAlias({ localPart: '', domain: '', displayName: '', shareSent: true, members: [] })).toEqual({
      localPart: '请输入邮箱地址',
      domain: '请选择域名',
    });
    expect(
      aliasInput({ localPart: ' Support ', domain: 'az.test', displayName: ' 客服 ', shareSent: false, members: [{ user_id: 1, can_send_as: true }] }),
    ).toEqual({ email: 'support@az.test', display_name: '客服', share_sent: false, members: [{ user_id: 1, can_send_as: true }] });
    expect(splitEmail('a.b@c.d')).toEqual(['a.b', 'c.d']);
    expect(splitEmail('nodomain')).toEqual(['nodomain', '']);
  });

  it('formats admin dates in the display timezone', () => {
    expect(formatDateTime(Date.UTC(2026, 9, 7, 12, 3, 9), 'Asia/Shanghai')).toBe('2026-10-07 20:03');
    expect(formatDateTime(Date.UTC(2026, 9, 7, 12, 3, 9), 'Asia/Shanghai', { seconds: true })).toBe('2026-10-07 20:03:09');
    expect(formatDateTime(null, 'Asia/Shanghai')).toBe('');
  });
});
