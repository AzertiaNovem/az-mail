// AZ Mail runtime configuration. Served per environment (Nginx: Cache-Control: no-store),
// so one frontend build works everywhere.
//   apiBase: ""  → same origin (dev: Vite proxies /api to 127.0.0.1:8080)
//   apiBase: "https://api.mail.example.com" → separately deployed API (CORS allowlisted)
// The WebSocket base is derived from apiBase (http→ws, https→wss).
window.__AZMAIL_CONFIG__ = { apiBase: "" };
