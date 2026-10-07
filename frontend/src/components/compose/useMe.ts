/** `['me']` query (current user, settings, identities) shared by the WP-F screens. */
import { useQuery } from '@tanstack/react-query';
import { getMe } from '@/api/endpoints';
import { queryKeys, staleTimes } from '@/api/queryKeys';

export function useMe() {
  return useQuery({ queryKey: queryKeys.me(), queryFn: ({ signal }) => getMe(signal), staleTime: staleTimes.me });
}
