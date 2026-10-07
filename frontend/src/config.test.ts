import { describe, expect, it } from 'vitest';
import { apiUrl, deriveWsBase, resolveApiUrl, resolveConfig, wsUrl } from './config';

const http = { protocol: 'http:', host: 'localhost:5173' };
const https = { protocol: 'https:', host: 'mail.example.com' };

describe('deriveWsBase', () => {
  it('uses the page origin when apiBase is empty', () => {
    expect(deriveWsBase('', http)).toBe('ws://localhost:5173');
    expect(deriveWsBase('', https)).toBe('wss://mail.example.com');
    expect(deriveWsBase('   ', https)).toBe('wss://mail.example.com');
  });

  it('maps http→ws and https→wss for absolute bases', () => {
    expect(deriveWsBase('http://127.0.0.1:8080', https)).toBe('ws://127.0.0.1:8080');
    expect(deriveWsBase('https://api.example.com', http)).toBe('wss://api.example.com');
    expect(deriveWsBase('HTTPS://API.example.com/', http)).toBe('wss://API.example.com');
  });

  it('keeps an explicit ws(s) base and handles protocol-relative and path bases', () => {
    expect(deriveWsBase('wss://rt.example.com', http)).toBe('wss://rt.example.com');
    expect(deriveWsBase('//api.example.com', https)).toBe('wss://api.example.com');
    expect(deriveWsBase('/mailapi/', https)).toBe('wss://mail.example.com/mailapi');
  });
});

describe('resolveConfig', () => {
  it('defaults to same origin when config.js is missing', () => {
    expect(resolveConfig(undefined, https)).toEqual({ apiBase: '', wsBase: 'wss://mail.example.com' });
  });

  it('strips trailing slashes and derives wsBase', () => {
    expect(resolveConfig({ apiBase: 'https://api.example.com/' }, http)).toEqual({
      apiBase: 'https://api.example.com',
      wsBase: 'wss://api.example.com',
    });
  });

  it('honours an explicit wsBase override', () => {
    expect(resolveConfig({ apiBase: 'https://api.example.com', wsBase: 'wss://ws.example.com/' }, http).wsBase).toBe(
      'wss://ws.example.com',
    );
  });
});

describe('URL helpers', () => {
  const cfg = resolveConfig({ apiBase: 'https://api.example.com' }, http);
  const same = resolveConfig({ apiBase: '' }, https);

  it('builds API and socket URLs', () => {
    expect(apiUrl('/api/threads', cfg)).toBe('https://api.example.com/api/threads');
    expect(apiUrl('api/threads', same)).toBe('/api/threads');
    expect(wsUrl(cfg)).toBe('wss://api.example.com/api/ws');
    expect(wsUrl(same)).toBe('wss://mail.example.com/api/ws');
  });

  it('prefixes root-relative server URLs and passes absolute ones through', () => {
    expect(resolveApiUrl('/api/files/1?d=a&exp=1&sig=x', cfg)).toBe('https://api.example.com/api/files/1?d=a&exp=1&sig=x');
    expect(resolveApiUrl('https://acct.r2.cloudflarestorage.com/b/k?X-Amz-Signature=1', cfg)).toBe(
      'https://acct.r2.cloudflarestorage.com/b/k?X-Amz-Signature=1',
    );
    expect(resolveApiUrl('data:image/png;base64,AA', cfg)).toBe('data:image/png;base64,AA');
  });
});
