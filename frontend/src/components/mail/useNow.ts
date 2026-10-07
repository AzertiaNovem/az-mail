import { useEffect, useState } from 'react';

/** Current time, refreshed every `intervalMs` (date columns roll over at midnight, "3 分钟前" ages). */
export function useNow(intervalMs = 60_000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return now;
}
