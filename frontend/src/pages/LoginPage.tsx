/**
 * Login [WP-E] (Gmail 2024 sign-in card): "登录" / "使用您的 AZ Mail 账号", email + password,
 * errors for invalid_credentials, too_many_attempts (with a retry_after countdown) and
 * account_disabled, then a redirect to `?next` (only same-origin paths starting with a single
 * "/") or the inbox.
 */
import { useQueryClient } from '@tanstack/react-query';
import { useEffect, useId, useState, type FormEvent } from 'react';
import { Navigate, useSearchParams } from 'react-router';
import { errorMessage, isApiError } from '@/api/client';
import { login } from '@/api/endpoints';
import type { TooManyAttemptsDetails } from '@/api/types';
import { Button, Checkbox, TextField } from '@/components/common';
import { LogoMark } from '@/components/layout/Logo';
import { t } from '@/i18n/zh';
import { completeLogin, useIsAuthenticated } from '@/stores/auth';

const HOME = '/mail/inbox';

/** The post-login destination: a same-origin path, never "//host", "/\\host" or /login itself. */
export function safeNext(next: string | null | undefined): string {
  if (!next) return HOME;
  if (!next.startsWith('/') || next.startsWith('//') || next.startsWith('/\\')) return HOME;
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f]/.test(next)) return HOME;
  if (next === '/login' || next.startsWith('/login?') || next.startsWith('/login/')) return HOME;
  return next;
}

/** "45 秒" / "3 分钟". */
export function formatWait(seconds: number): string {
  const s = Math.max(1, Math.ceil(seconds));
  return s < 60 ? t('mail.login.seconds', { n: s }) : t('mail.login.minutes', { n: Math.ceil(s / 60) });
}

type FieldError = { field: 'email' | 'password' | 'form'; message: string };

export function LoginPage() {
  const qc = useQueryClient();
  const [params] = useSearchParams();
  const next = safeNext(params.get('next'));
  const authed = useIsAuthenticated();
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);
  const [error, setError] = useState<FieldError | null>(null);
  const [busy, setBusy] = useState(false);
  const [retryUntil, setRetryUntil] = useState<number | null>(null);
  const [now, setNow] = useState(() => Date.now());
  const formId = useId();

  useEffect(() => {
    document.title = `${t('mail.login.title')} - AZ Mail`;
  }, []);

  useEffect(() => {
    if (retryUntil === null) return;
    const id = setInterval(() => {
      const n = Date.now();
      setNow(n);
      if (n >= retryUntil) {
        setRetryUntil(null);
        setError(null);
      }
    }, 1000);
    return () => clearInterval(id);
  }, [retryUntil]);

  // Signed in (here — completeLogin stores the token — or in another tab): go on.
  if (authed) return <Navigate to={next} replace />;

  const waiting = retryUntil !== null && now < retryUntil;

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    if (busy || waiting) return;
    const addr = email.trim();
    if (!addr) {
      setError({ field: 'email', message: t('mail.login.emailRequired') });
      return;
    }
    if (!password) {
      setError({ field: 'password', message: t('mail.login.passwordRequired') });
      return;
    }
    setBusy(true);
    setError(null);
    try {
      const res = await login({ email: addr, password });
      completeLogin(qc, res);
    } catch (err) {
      if (isApiError(err, 'too_many_attempts')) {
        const s = Number(err.detailsAs<TooManyAttemptsDetails>().retry_after);
        const wait = Number.isFinite(s) && s > 0 ? s : 60;
        setRetryUntil(Date.now() + wait * 1000);
        setNow(Date.now());
        setError({ field: 'form', message: t('mail.login.retryIn', { time: formatWait(wait) }) });
      } else if (isApiError(err, 'invalid_credentials')) {
        setError({ field: 'password', message: t('errors.invalid_credentials') });
        setPassword('');
      } else if (isApiError(err, 'account_disabled')) {
        setError({ field: 'form', message: t('errors.account_disabled') });
      } else {
        setError({ field: 'form', message: errorMessage(err) });
      }
    } finally {
      setBusy(false);
    }
  };

  const formMessage =
    error?.field === 'form'
      ? waiting && retryUntil !== null
        ? t('mail.login.retryIn', { time: formatWait((retryUntil - now) / 1000) })
        : error.message
      : null;

  return (
    <main className="flex min-h-dvh flex-col items-center justify-center bg-[#f0f4f9] px-4 py-8">
      <div className="w-full max-w-[1040px] rounded-[28px] bg-surface-container p-6 shadow-[0_1px_2px_rgb(0_0_0/0.04)] sm:p-9">
        <div className="grid gap-8 md:grid-cols-2 md:gap-12">
          <div>
            <LogoMark size={48} />
            <h1 className="mt-6 text-[36px] font-normal leading-[44px] text-on-surface">{t('mail.login.title')}</h1>
            <p className="mt-4 text-base text-on-surface">{t('mail.login.subtitle')}</p>
          </div>
          <form id={formId} onSubmit={(e) => void submit(e)} noValidate className="flex flex-col gap-5 md:pt-[86px]">
            <TextField
              label={t('mail.login.email')}
              type="email"
              name="email"
              autoComplete="username"
              inputMode="email"
              autoFocus
              value={email}
              onChange={(e) => {
                setEmail(e.target.value);
                if (error?.field === 'email') setError(null);
              }}
              error={error?.field === 'email' ? error.message : undefined}
              disabled={busy}
              className="text-base"
              containerClassName="[&_.h-10]:h-14"
            />
            <TextField
              label={t('mail.login.password')}
              type={showPassword ? 'text' : 'password'}
              name="password"
              autoComplete="current-password"
              value={password}
              onChange={(e) => {
                setPassword(e.target.value);
                if (error?.field === 'password') setError(null);
              }}
              error={error?.field === 'password' ? error.message : undefined}
              disabled={busy}
              className="text-base"
              containerClassName="[&_.h-10]:h-14"
            />
            <Checkbox checked={showPassword} onCheckedChange={setShowPassword} label={t('mail.login.showPassword')} />
            {formMessage && (
              <p role="alert" className="rounded-lg bg-error-container px-3 py-2 text-sm text-on-error-container">
                {formMessage}
              </p>
            )}
            <div className="mt-4 flex justify-end">
              <Button type="submit" loading={busy} disabled={waiting} className="min-w-[96px]">
                {t('mail.login.submit')}
              </Button>
            </div>
          </form>
        </div>
      </div>
      <p className="mt-6 text-xs text-on-surface-variant">{t('mail.login.footer')}</p>
    </main>
  );
}
