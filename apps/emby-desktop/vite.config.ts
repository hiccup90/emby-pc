import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  build: {
    rollupOptions: {
      input: {
        // Main library window + the transparent playback overlay window
        // (`controls.html` is opened by the Rust host at `WebviewUrl::App`).
        main: fileURLToPath(new URL("index.html", import.meta.url)),
        controls: fileURLToPath(new URL("controls.html", import.meta.url)),
      },
    },
  },
  server: {
    host: "127.0.0.1",
    port: 1420,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
});
