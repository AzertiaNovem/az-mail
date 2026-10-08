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
    rolldownOptions: {
      output: {
        codeSplitting: {
          // The rich-text editor (TipTap / ProseMirror) is only used by the lazily loaded compose
          // window and the settings signature editor: one named, cacheable chunk that the
          // initial (inbox) load never fetches.
          groups: [
            {
              name: 'editor',
              // Only the editor packages themselves: their shared dependencies (react…) stay in
              // the entry, otherwise the entry would import this chunk.
              includeDependenciesRecursively: false,
              test: /[\\/]node_modules[\\/](?:@tiptap[\\/]|prosemirror-|orderedmap[\\/]|rope-sequence[\\/]|w3c-keyname[\\/]|linkifyjs[\\/])/,
            },
          ],
        },
      },
    },
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
