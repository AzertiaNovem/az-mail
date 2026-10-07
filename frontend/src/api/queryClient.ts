/**
 * The app's single QueryClient. Exported as a module singleton so non-React code
 * (WebSocket invalidation, auth events) can reach the cache.
 */
import { QueryClient } from '@tanstack/react-query';
import { ApiError } from './client';

/** Retry once, and only for transient failures (network errors, 5xx). Never retry 4xx. */
export function shouldRetry(failureCount: number, error: unknown): boolean {
  if (failureCount >= 1) return false;
  if (error instanceof ApiError) return error.status === 0 || error.status >= 500;
  return false;
}

export function createQueryClient(): QueryClient {
  return new QueryClient({
    defaultOptions: {
      queries: {
        retry: shouldRetry,
        refetchOnWindowFocus: true,
        refetchOnReconnect: true,
      },
      mutations: {
        retry: false,
      },
    },
  });
}

export const queryClient = createQueryClient();
