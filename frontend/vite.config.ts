/// <reference types="vitest/config" />
import { fileURLToPath, URL } from 'node:url';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// Dev: the SPA runs on :5173 and proxies /api (REST + WebSocket) to the backend on :8080,
// so the default runtime config (apiBase "" = same origin) works unchanged.
// Prod: one build for every environment; /config.js (served per environment) sets apiBase.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  server: {
    port: 5173,
    strictPort: true,
    proxy: {
      '/api': { target: 'http://127.0.0.1:8080', ws: true },
    },
  },
  // `vite preview` reuses server.proxy.
  preview: { port: 4173 },
  build: {
    target: 'es2022',
    sourcemap: true,
    // The self-hosted Material Symbols font is ~4 MB; keep it a separate file, never inlined.
    assetsInlineLimit: 4096,
    // No manual chunk groups: forcing the TipTap packages into a named chunk made Rolldown emit
    // a circular import between that chunk and a lazy page chunk (LinkDialog <-> editor), which
    // crashes at startup ("Cannot read properties of undefined (reading 'extend')"). Natural
    // dynamic-import splitting already keeps the editor out of the initial load (compose, settings
    // and admin are React.lazy). `pnpm check:chunks` fails the build on any chunk import cycle.
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    css: false,
    restoreMocks: true,
    unstubGlobals: true,
  },
});
