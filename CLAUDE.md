# AZ Mail — project guide for agents

Team-internal webmail with a Gmail-like UI. All mail is sent and received through the **Resend API**; attachments and raw .eml blobs live in **Cloudflare R2** (S3 API, SigV4) in production, local disk in dev/tests. Frontend and backend deploy separately.

## Layout
- `backend/` — C++20, Boost.Beast/Asio (C++20 coroutines), Boost.JSON/URL/Program_options, OpenSSL 3, SQLite3 (WAL, FTS5 trigram). CMake project root. Library `azmail_lib` (GLOB of `src/**/*.cpp` minus `main.cpp`), executable `azmail`, tests `azmail_tests` (Catch2 v3, `tests/unit/test_*.cpp`).
- `frontend/` — React 19 + TypeScript + Vite, Tailwind v4, TanStack Query v5, react-router, TipTap v3, DOMPurify, zustand, Radix. UI text is Simplified Chinese.
- `tools/mock_resend/` — Python stdlib mock of Resend (+ minimal S3/R2) used by E2E and dev.
- `tests/e2e/` — Python stdlib end-to-end scenarios.
- `deploy/`, `scripts/`, `docs/`.

## Contracts (read before coding)
- `docs/DESIGN.md` — frozen design spec: schema (§2), source tree + C++ interfaces (§3), REST/WS (§4), frontend (§5), mock/E2E (§6), work packages (§7), addenda at the end (R2 storage). Changes must be additive.
- `docs/CONTRACTS.md` — file → owner table, route → handler → functions, job kinds, WS emitters, decisions made during WP0.
- `docs/API.md` — wire format (read Addendum B: bare arrays, 204 on DELETE, flat WS frames, error details); `frontend/src/api/types.ts` mirrors it exactly.
- Contract headers in `backend/src/**.hpp` are frozen: only additive changes by the owning work package.

## Build & test
Backend (use your OWN build dir when other agents may be building concurrently):
```
cmake -S backend -B backend/build/dev --preset mac-debug   # or: cmake --preset mac-debug
cmake --build backend/build/dev -j
ctest --test-dir backend/build/dev --output-on-failure
```
Homebrew paths: SQLite is keg-only (`/opt/homebrew/opt/sqlite`), OpenSSL `/opt/homebrew/opt/openssl@3`; the presets set these.
Frontend: `cd frontend && pnpm install && pnpm build && pnpm test && pnpm lint` (lint = oxlint; TypeScript 7 has no typescript-eslint). react-router v8 data mode has no regex params — validate `:folder`/`:tab` in components. i18n: `zh.ts` is shared/frozen; WP-E adds keys only in `zh.mail.ts`, WP-F only in `zh.compose.ts`.
E2E: `python3 tests/e2e/run.py` (starts mock + backend on random ports).

## Non-negotiable rules
- Namespace `azm::<module>`. C++20. Define `BOOST_ASIO_NO_DEPRECATED`; always pass completion tokens explicitly; use `asio::ssl::stream<beast::tcp_stream>` (not `beast::ssl_stream`).
- Every mail/domain function takes `owner_id` explicitly and every query filters on it (IDOR).
- Never do network I/O (Resend, R2, downloads) inside a DB transaction. Write transactions use `Pool::write` (BEGIN IMMEDIATE + BUSY retry).
- WebSocket events are published only via `tx.emit(...)` (flushed after COMMIT), never from inside a transaction directly.
- Stored HTML references local attachments as `cid:<content_id>`; signed URLs are produced only at read time.
- Times are ms-epoch UTC integers in DB and API.
- API errors: `{"error":{"code":"snake_case","message":"中文说明","details":{}}}` via `ApiError`.
- Never log secrets, tokens, signed-URL query strings, or mail bodies.
- Tests: each module ships Catch2 tests; keep them hermetic (temp dirs, in-memory/temp SQLite, no real network).
- Only touch files your work package owns (DESIGN.md §7 + addenda). Do not edit CMake globs, contracts, or other packages' files; if a contract is insufficient, make the smallest additive change and report it.
