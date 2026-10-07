import './styles/index.css';
import { QueryClientProvider } from '@tanstack/react-query';
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { RouterProvider } from 'react-router/dom';
import { getMe } from '@/api/endpoints';
import { queryClient } from '@/api/queryClient';
import { queryKeys, staleTimes } from '@/api/queryKeys';
import { ToastHost, TooltipProvider } from '@/components/common';
import { t } from '@/i18n/zh';
import { installAuthStorageSync, installComposeOwnerSync, onExternalLogin, onUnauthorized } from '@/stores/auth';
import { useComposeStore } from '@/stores/compose';
import { toast } from '@/stores/toast';
import { router } from './router';

// ── session lifecycle ──
installAuthStorageSync();
// Compose windows restored from sessionStorage close when ['me'] turns out to be another user.
installComposeOwnerSync(queryClient);

onUnauthorized((reason) => {
  queryClient.clear();
  if (reason !== 'expired') useComposeStore.getState().closeAll();
  if (reason === 'expired' || reason === 'revoked') toast.push({ message: t('toasts.sessionExpired') });
  else if (reason === 'other_tab') toast.push({ message: t('toasts.loggedOutElsewhere') });

  const { pathname, search, hash } = router.state.location;
  if (pathname !== '/login') {
    // Return to the current page only when the same person is likely to log back in (session
    // expired / revoked). After an explicit logout, or a logout in another tab, the next person
    // to log in here must not land on the previous user's thread URL (404 for them).
    const keepPlace = (reason === 'expired' || reason === 'revoked') && pathname !== '/';
    const next = keepPlace ? `?next=${encodeURIComponent(pathname + search + hash)}` : '';
    void router.navigate(`/login${next}`, { replace: true });
  }
});

// Another tab logged in (possibly as someone else): refetch everything with the new token, and
// make sure ['me'] is refetched even when nothing observes it so the compose owner check runs.
onExternalLogin(() => {
  void queryClient
    .resetQueries()
    .then(() =>
      queryClient.fetchQuery({ queryKey: queryKeys.me(), queryFn: ({ signal }) => getMe(signal), staleTime: staleTimes.me }),
    )
    .catch(() => {
      /* a 401 is handled by onUnauthorized; anything else retries on the next ['me'] fetch */
    });
});

const container = document.getElementById('root');
if (!container) throw new Error('#root not found');

createRoot(container).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <TooltipProvider>
        <RouterProvider router={router} />
        <ToastHost />
      </TooltipProvider>
    </QueryClientProvider>
  </StrictMode>,
);
