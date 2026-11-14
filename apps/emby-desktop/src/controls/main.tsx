import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import Overlay from "./Overlay";
import {
  OVERLAY_BAR_HEIGHT,
  OVERLAY_PANEL_HEIGHT,
  OVERLAY_TOP_BAND_HEIGHT,
  OVERLAY_TOP_BAND_MAX_WIDTH,
} from "./contract";

if (typeof document !== "undefined" && !document.querySelector('link[rel="icon"]')) {
  const icon = document.createElement("link");
  icon.rel = "icon";
  icon.href = "data:,";
  document.head.append(icon);
}

/**
 * `contract.ts` owns the overlay layout numbers; publish them as CSS variables
 * so `overlay.css` can lay the bar and the panel out without repeating them.
 * Rust keeps the same values for the actual window geometry.
 */
const root = document.documentElement;
root.style.setProperty("--ov-bar-h", `${OVERLAY_BAR_HEIGHT}px`);
root.style.setProperty("--ov-panel-h", `${OVERLAY_PANEL_HEIGHT}px`);
root.style.setProperty("--ov-top-h", `${OVERLAY_TOP_BAND_HEIGHT}px`);
root.style.setProperty("--ov-top-max-w", `${OVERLAY_TOP_BAND_MAX_WIDTH}px`);

const rootElement = document.getElementById("overlay-root");
if (!rootElement) {
  throw new Error("Overlay root element #overlay-root was not found");
}

createRoot(rootElement).render(
  <StrictMode>
    <Overlay />
  </StrictMode>,
);