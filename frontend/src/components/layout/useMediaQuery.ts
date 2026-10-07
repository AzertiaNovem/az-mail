import { useSyncExternalStore } from 'react';

/** Breakpoints of the mail layout. */
export const NARROW_QUERY = '(max-width: 1023px)'; // sidebar collapses to a rail
export const MOBILE_QUERY = '(max-width: 767px)'; // sidebar becomes a drawer; rows go two-line

function mql(query: string): MediaQueryList | null {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return null;
  try {
    return window.matchMedia(query);
  } catch {
    return null;
  }
}

/** Subscribes to a media query; `fallback` where matchMedia is unavailable (tests, SSR). */
export function useMediaQuery(query: string, fallback = false): boolean {
  return useSyncExternalStore(
    (onChange) => {
      const m = mql(query);
      if (!m) return () => {};
      m.addEventListener('change', onChange);
      return () => m.removeEventListener('change', onChange);
    },
    () => mql(query)?.matches ?? fallback,
    () => fallback,
  );
}

export const useIsNarrow = () => useMediaQuery(NARROW_QUERY);
export const useIsMobile = () => useMediaQuery(MOBILE_QUERY);
