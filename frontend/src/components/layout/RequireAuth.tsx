/**
 * Route guard [WP-E] (DESIGN.md §5 "RequireAuth loads ['me']"): no token → /login?next=…;
 * otherwise waits for `['me']` (a 401 ends the session through the API client, which sends the
 * app to /login) and renders the child routes.
 */
import { useQuery } from '@tanstack/react-query';
import { Navigate, Outlet, useLocation } from 'react-router';
import { isApiError } from '@/api/client';
import { getMe } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import { Button, Spinner } from '@/components/common';
import { t } from '@/i18n/zh';
import { useIsAuthenticated } from '@/stores/auth';
import { LogoMark } from './Logo';

export function FullPageLoading({ label = t('mail.shell.loadingApp') }: { label?: string }) {
  return (
    <div className="flex h-dvh flex-col items-center justify-center gap-6 bg-surface" role="status" aria-label={label}>
      <LogoMark size={72} />
      <Spinner size={28} label={label} />
    </div>
  );
}

/** `next` value for a location ("" for the default landing page). */
export function nextParam(pathname: string, search: string, hash: string): string {
  const path = pathname + search + hash;
  return pathname === '/' || pathname === '/mail/inbox' ? '' : `?next=${encodeURIComponent(path)}`;
}

export function RequireAuth() {
  const authed = useIsAuthenticated();
  const location = useLocation();
  const me = useQuery({
    queryKey: queryKeys.me(),
    queryFn: ({ signal }) => getMe(signal),
    staleTime: staleTimes.me,
    enabled: authed,
  });

  if (!authed) return <Navigate to={`/login${nextParam(location.pathname, location.search, location.hash)}`} replace />;
  if (me.data) return <Outlet />;
  if (me.isError && !(isApiError(me.error) && me.error.status === 401)) {
    return (
      <div className="flex h-dvh flex-col items-center justify-center gap-4 bg-surface text-on-surface-variant" role="alert">
        <LogoMark size={56} />
        <p>{t('mail.shell.loadFailed')}</p>
        <Button variant="tonal" icon="refresh" onClick={() => void me.refetch()}>
          {t('mail.list.retry')}
        </Button>
      </div>
    );
  }
  return <FullPageLoading />;
}
