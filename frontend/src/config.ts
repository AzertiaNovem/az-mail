/**
 * Runtime configuration (DESIGN.md §1 E2).
 *
 * `/config.js` is loaded before the bundle and sets `window.__AZMAIL_CONFIG__ = { apiBase }`.
 * An empty apiBase means "same origin" (dev uses the Vite proxy; prod may put the API behind
 * the same Nginx). The WebSocket base is derived from apiBase: http→ws, https→wss.
 */

export interface RawRuntimeConfig {
  /** "" (same origin), an absolute origin ("https://api.example.com") or a path prefix ("/mailapi"). */
  apiBase?: string;
  /** Optional explicit override; normally derived from apiBase. */
  wsBase?: string;
}

export interface RuntimeConfig {
  /** Prefix for REST paths ("/api/..."), without a trailing slash. "" = same origin. */
  apiBase: string;
  /** Absolute ws:// or wss:// base, without a trailing slash; the socket URL is `${wsBase}/api/ws`. */
  wsBase: string;
}

declare global {
  interface Window {
    __AZMAIL_CONFIG__?: RawRuntimeConfig;
  }
}

/** The parts of `window.location` the derivation needs (injectable for tests). */
export interface LocationLike {
  protocol: string;
  host: string;
}

const stripTrailingSlash = (s: string): string => s.replace(/\/+$/, '');

/** Derives the WebSocket base for an apiBase relative to the page location. */
export function deriveWsBase(apiBase: string, location: LocationLike): string {
  const base = stripTrailingSlash(apiBase.trim());
  const pageWs = location.protocol === 'https:' ? 'wss:' : 'ws:';
  if (base === '') return `${pageWs}//${location.host}`;
  if (/^https:\/\//i.test(base)) return base.replace(/^https:/i, 'wss:');
  if (/^http:\/\//i.test(base)) return base.replace(/^http:/i, 'ws:');
  if (/^wss?:\/\//i.test(base)) return base;
  if (base.startsWith('//')) return `${pageWs}${base}`; // protocol-relative
  // Path prefix on the same origin ("/mailapi").
  return `${pageWs}//${location.host}${base.startsWith('/') ? base : `/${base}`}`;
}

export function resolveConfig(raw: RawRuntimeConfig | undefined, location: LocationLike): RuntimeConfig {
  const apiBase = stripTrailingSlash((raw?.apiBase ?? '').trim());
  const override = raw?.wsBase?.trim();
  const wsBase = override ? stripTrailingSlash(override) : deriveWsBase(apiBase, location);
  return { apiBase, wsBase };
}

export const config: RuntimeConfig = resolveConfig(
  typeof window !== 'undefined' ? window.__AZMAIL_CONFIG__ : undefined,
  typeof window !== 'undefined' ? window.location : { protocol: 'http:', host: 'localhost' },
);

/** Absolute-or-same-origin URL for an API path such as "/api/threads". */
export function apiUrl(path: string, cfg: RuntimeConfig = config): string {
  return `${cfg.apiBase}${path.startsWith('/') ? path : `/${path}`}`;
}

/** URL of the realtime socket. */
export function wsUrl(cfg: RuntimeConfig = config): string {
  return `${cfg.wsBase}/api/ws`;
}

/**
 * Resolves a server-produced URL (signed file URLs, raw_url, …). Absolute URLs pass through;
 * root-relative API URLs ("/api/files/…") are prefixed with apiBase.
 */
export function resolveApiUrl(url: string, cfg: RuntimeConfig = config): string {
  if (/^[a-z][a-z0-9+.-]*:/i.test(url) || url.startsWith('//')) return url;
  return apiUrl(url, cfg);
}
