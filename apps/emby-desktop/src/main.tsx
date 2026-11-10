import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import App from "./App";

async function start() {
  // `pnpm dev:mock` only: a fictional Emby library in a plain browser. The
  // condition is a build-time constant, so production bundles drop the mock.
  if (import.meta.env.MODE === "mock") {
    const { installMockTauri } = await import("./dev/mockTauri");
    installMockTauri();
  }

  const rootElement = document.getElementById("root");
  if (!rootElement) {
    throw new Error("Root element #root was not found");
  }

  createRoot(rootElement).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}

void start();
