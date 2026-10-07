/**
 * Admin tab page [WP-F]. PLACEHOLDER written by WP0 so the router (WP-E) builds; WP-F replaces it.
 * Contract (kept by the replacement): `export function AdminStats()`, no props, rendered inside
 * AdminLayout's `<Outlet/>`. Data comes from api/admin.ts (`adminKeys` + admin* calls).
 * Note: api/types.ts also exports an `AdminStats` interface; import it aliased here
 * (`import type { AdminStats as AdminStatsData } from '@/api/types'`).
 */
export function AdminStats() {
  return <p className="text-on-surface-variant">统计</p>;
}
