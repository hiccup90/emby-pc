import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import Overlay from "./Overlay";

if (typeof document !== "undefined" && !document.querySelector('link[rel="icon"]')) {
  const icon = document.createElement("link");
  icon.rel = "icon";
  icon.href = "data:,";
  document.head.append(icon);
}

const rootElement = document.getElementById("overlay-root");
if (!rootElement) {
  throw new Error("Overlay root element #overlay-root was not found");
}

createRoot(rootElement).render(
  <StrictMode>
    <Overlay />
  </StrictMode>,
);
