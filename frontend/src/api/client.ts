/**
 * HTTP client for the AZ Mail API.
 *
 * - Base URL from runtime config (`apiBase`, "" = same origin).
 * - `Authorization: Bearer <token>` from the auth store (localStorage `azmail.token`).
 * - JSON in/out; 204 / empty bodies resolve to `undefined`.
 * - Errors: every failure rejects with `ApiError` (parsed from `{"error":{code,message,details}}`),
 *   except aborts, which reject with the original `AbortError` DOMException.
 * - 401 on an authenticated request clears the token and emits `unauthorized` — except a 401
 *   `invalid_credentials` (a credential check such as a wrong current password), which is an
 *   ordinary error and leaves the session alone (`endsSession`).
 * - `uploadRaw` uses XMLHttpRequest for upload progress (POST /api/attachments).
 */
import { apiUrl } from '@/config';
import { errorCodeMessage, t } from '@/i18n/zh';
import { getToken, handleUnauthorizedResponse } from '@/stores/auth';
import type { ApiErrorBody, ApiErrorCode } from './types';

export { onUnauthorized } from '@/stores/auth';

export class ApiError extends Error {
  /** HTTP status; 0 for network / client-side failures. */
  readonly status: number;
  readonly code: ApiErrorCode;
  readonly details: Record<string, unknown>;

  constructor(status: number, code: ApiErrorCode, message: string, details: Record<string, unknown> = {}) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
    this.details = details;
  }

  /** Typed access to `details` for a known error code. */
  detailsAs<T>(): Partial<T> {
    return this.details as Partial<T>;
  }
}

export function isApiError(e: unknown, code?: ApiErrorCode): e is ApiError {
  return e instanceof ApiError && (code === undefined || e.code === code);
}

export function isAbortError(e: unknown): boolean {
  return typeof e === 'object' && e !== null && (e as { name?: unknown }).name === 'AbortError';
}

/** User-facing Chinese message for any thrown value. */
export function errorMessage(e: unknown): string {
  if (e instanceof ApiError) return e.message;
  return t('errors.unknown');
}

// ───────────── query strings ─────────────

export type QueryValue = string | number | boolean | null | undefined;
export type QueryParams = Record<string, QueryValue>;

/**
 * Encodes query params with encodeURIComponent (space → %20, '+' → %2B), skipping null/undefined
 * and empty strings. Booleans become 1/0 (the API's flag convention, e.g. `inline=0|1`).
 */
export function buildQuery(params: QueryParams | undefined): string {
  if (!params) return '';
  const parts: string[] = [];
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined || v === null || v === '') continue;
    const s = typeof v === 'boolean' ? (v ? '1' : '0') : String(v);
    parts.push(`${encodeURIComponent(k)}=${encodeURIComponent(s)}`);
  }
  return parts.length ? `?${parts.join('&')}` : '';
}

// ───────────── error parsing ─────────────

function fallbackMessage(status: number): string {
  if (status === 401) return t('errors.unauthorized');
  if (status === 403) return t('errors.forbidden');
  if (status === 404) return t('errors.not_found');
  if (status === 413) return t('errors.payload_too_large');
  if (status === 429) return t('errors.too_many_attempts');
  if (status === 503) return t('errors.service_unavailable');
  if (status >= 500) return t('errors.server');
  return t('errors.unknown');
}

function isErrorBody(v: unknown): v is ApiErrorBody {
  if (typeof v !== 'object' || v === null) return false;
  const err = (v as { error?: unknown }).error;
  return typeof err === 'object' && err !== null && typeof (err as { code?: unknown }).code === 'string';
}

/** Builds an ApiError from a non-2xx status and its (possibly empty / non-JSON) body text. */
export function parseErrorResponse(status: number, bodyText: string): ApiError {
  let parsed: unknown;
  try {
    parsed = bodyText ? JSON.parse(bodyText) : undefined;
  } catch {
    parsed = undefined;
  }
  if (isErrorBody(parsed)) {
    const { code, message, details } = parsed.error;
    const msg = typeof message === 'string' && message ? message : (errorCodeMessage(code) ?? fallbackMessage(status));
    const det = typeof details === 'object' && details !== null ? details : {};
    return new ApiError(status, code, msg, det);
  }
  return new ApiError(status, `http_${status}`, fallbackMessage(status));
}

const networkError = () => new ApiError(0, 'network_error', t('errors.network'));

/**
 * Whether an error response to an authenticated request means the session is gone. A 401
 * `invalid_credentials` is a failed credential check (e.g. POST /api/auth/password with a wrong
 * current_password), not an expired token, so it must not log the user out.
 */
export function endsSession(err: ApiError): boolean {
  return err.status === 401 && err.code !== 'invalid_credentials';
}

// ───────────── fetch wrapper ─────────────

export type ResponseKind = 'json' | 'text' | 'blob';

export interface RequestOptions {
  method?: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE';
  query?: QueryParams;
  /** JSON-encoded unless it is a Blob / FormData / string. */
  body?: unknown;
  signal?: AbortSignal;
  headers?: Record<string, string>;
  /** Send the bearer token (default true). */
  auth?: boolean;
  /** How to decode a 2xx body (default json; empty body → undefined). */
  responseType?: ResponseKind;
}

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const { method = 'GET', query, body, signal, auth = true, responseType = 'json' } = opts;
  const headers: Record<string, string> = { Accept: 'application/json', ...opts.headers };
  const token = auth ? getToken() : null;
  if (token) headers.Authorization = `Bearer ${token}`;

  let payload: BodyInit | undefined;
  if (body !== undefined) {
    if (body instanceof Blob || body instanceof FormData || typeof body === 'string') {
      payload = body;
    } else {
      payload = JSON.stringify(body);
      headers['Content-Type'] = 'application/json';
    }
  }

  let res: Response;
  try {
    res = await fetch(apiUrl(path) + buildQuery(query), { method, headers, body: payload, signal });
  } catch (e) {
    if (isAbortError(e) || signal?.aborted) throw e;
    throw networkError();
  }

  if (!res.ok) {
    let text = '';
    try {
      text = await res.text();
    } catch (e) {
      if (isAbortError(e)) throw e;
    }
    const err = parseErrorResponse(res.status, text);
    if (token && endsSession(err)) handleUnauthorizedResponse(token);
    throw err;
  }

  if (res.status === 204 || res.status === 205) return undefined as T;
  try {
    if (responseType === 'blob') return (await res.blob()) as T;
    const text = await res.text();
    if (responseType === 'text') return text as T;
    if (!text) return undefined as T;
    return JSON.parse(text) as T;
  } catch (e) {
    if (isAbortError(e)) throw e;
    throw new ApiError(res.status, 'invalid_response', t('errors.invalid_response'));
  }
}

type BodyOpts = Omit<RequestOptions, 'method' | 'body'>;

export const api = {
  get: <T>(path: string, opts?: BodyOpts) => request<T>(path, { ...opts, method: 'GET' }),
  post: <T>(path: string, body?: unknown, opts?: BodyOpts) => request<T>(path, { ...opts, method: 'POST', body }),
  put: <T>(path: string, body?: unknown, opts?: BodyOpts) => request<T>(path, { ...opts, method: 'PUT', body }),
  patch: <T>(path: string, body?: unknown, opts?: BodyOpts) => request<T>(path, { ...opts, method: 'PATCH', body }),
  delete: <T = void>(path: string, opts?: BodyOpts) => request<T>(path, { ...opts, method: 'DELETE' }),
};

// ───────────── raw upload with progress (XHR) ─────────────

export interface UploadProgress {
  loaded: number;
  /** 0 when not computable. */
  total: number;
  /** 0..1 */
  fraction: number;
}

export interface UploadOptions {
  query?: QueryParams;
  /** Defaults to the Blob's type, else application/octet-stream. */
  contentType?: string;
  onProgress?: (p: UploadProgress) => void;
  signal?: AbortSignal;
}

/** POSTs a raw body (not multipart) and resolves with the decoded JSON response. */
export function uploadRaw<T>(path: string, body: Blob, opts: UploadOptions = {}): Promise<T> {
  const { query, onProgress, signal } = opts;
  const contentType = opts.contentType || body.type || 'application/octet-stream';
  const token = getToken();

  return new Promise<T>((resolve, reject) => {
    if (signal?.aborted) {
      reject(signal.reason ?? new DOMException('Aborted', 'AbortError'));
      return;
    }
    const xhr = new XMLHttpRequest();
    xhr.open('POST', apiUrl(path) + buildQuery(query));
    xhr.setRequestHeader('Accept', 'application/json');
    xhr.setRequestHeader('Content-Type', contentType);
    if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`);

    const onAbortSignal = () => xhr.abort();
    signal?.addEventListener('abort', onAbortSignal, { once: true });
    const cleanup = () => signal?.removeEventListener('abort', onAbortSignal);

    if (onProgress) {
      xhr.upload.onprogress = (ev) => {
        const total = ev.lengthComputable ? ev.total : body.size;
        onProgress({ loaded: ev.loaded, total, fraction: total > 0 ? Math.min(1, ev.loaded / total) : 0 });
      };
    }
    xhr.onload = () => {
      cleanup();
      const text = typeof xhr.responseText === 'string' ? xhr.responseText : '';
      if (xhr.status >= 200 && xhr.status < 300) {
        if (!text) return resolve(undefined as T);
        try {
          resolve(JSON.parse(text) as T);
        } catch {
          reject(new ApiError(xhr.status, 'invalid_response', t('errors.invalid_response')));
        }
        return;
      }
      if (xhr.status === 0) return reject(networkError());
      const err = parseErrorResponse(xhr.status, text);
      if (token && endsSession(err)) handleUnauthorizedResponse(token);
      reject(err);
    };
    xhr.onerror = () => {
      cleanup();
      reject(networkError());
    };
    xhr.ontimeout = xhr.onerror;
    xhr.onabort = () => {
      cleanup();
      reject(new DOMException('Aborted', 'AbortError'));
    };
    xhr.send(body);
  });
}
