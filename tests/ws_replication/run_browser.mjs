#!/usr/bin/env node
/**
 * Browser runner for the WebSocket replication test.
 *
 * 1. Starts the native WS server (ReplicatingMapStorage sender)
 * 2. Serves WASM artifacts + test.html on an HTTP port
 * 3. Opens the browser at the test page
 *
 * Usage:
 *   cd tests/ws_replication && npm install
 *   node run_browser.mjs
 *   node run_browser.mjs --ws-port 19876 --http-port 8080
 *
 * Requirements:
 *   - Native server built:      cmake --build build -j --target ws_replication_server
 *   - Leaves JS library built:  cmake --build build-wasm -j --target leaves_js_output
 *   - Browser with SharedArrayBuffer support (Chrome 92+, Firefox 90+, Safari 15.2+)
 */

import { spawn, execSync } from "node:child_process";
import { createServer } from "node:http";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";
import { fileURLToPath } from "node:url";
import { dirname } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT      = join(__dirname, "..", "..");

// ── Parse args ──────────────────────────────────────────────────
const args = process.argv.slice(2);
function argVal(name, fallback) {
  const idx = args.indexOf(name);
  return idx >= 0 && idx + 1 < args.length ? args[idx + 1] : fallback;
}

const SERVER_BIN = argVal("--server", join(ROOT, "build", "ws_replication_server"));
const WASM_DIR   = argVal("--wasm-dir", join(ROOT, "js"));
const WS_PORT    = parseInt(argVal("--ws-port", "19876"), 10);
const HTTP_PORT  = parseInt(argVal("--http-port", "8080"), 10);

// ── Temp dir for server DB ──────────────────────────────────────
const tmp    = mkdtempSync(join(tmpdir(), "leaves-ws-browser-"));
const dbPath = join(tmp, "src.lvs");

function cleanup() {
  try { rmSync(tmp, { recursive: true, force: true }); } catch {}
}

// ── MIME types ──────────────────────────────────────────────────
const MIME = {
  ".html": "text/html",
  ".js":   "application/javascript",
  ".wasm": "application/wasm",
  ".css":  "text/css",
};

// ── Static file server ─────────────────────────────────────────
function startHttpServer() {
  return new Promise((resolve) => {
    const server = createServer((req, res) => {
      const url = new URL(req.url, `http://localhost:${HTTP_PORT}`);
      let filePath;

      // Serve test.html and the HTML from this directory
      if (url.pathname === "/" || url.pathname === "/test.html") {
        filePath = join(__dirname, "test.html");
      } else if (url.pathname === "/leaves.js") {
        filePath = join(WASM_DIR, "leaves.js");
      } else if (url.pathname === "/leaves.wasm") {
        filePath = join(WASM_DIR, "leaves.wasm");
      } else if (url.pathname === "/leaves_replication.js") {
        filePath = join(WASM_DIR, "leaves_replication.js");
      } else {
        // Try WASM dir for other assets
        filePath = join(WASM_DIR, url.pathname.slice(1));
      }

      try {
        const data = readFileSync(filePath);
        const ext = extname(filePath);
        res.writeHead(200, {
          "Content-Type": MIME[ext] || "application/octet-stream",
          "Cross-Origin-Opener-Policy": "same-origin",
          "Cross-Origin-Embedder-Policy": "require-corp",
          "Cache-Control": "no-cache, no-store, must-revalidate",
          "Pragma": "no-cache",
          "Expires": "0",
        });
        res.end(data);
      } catch {
        res.writeHead(404);
        res.end("Not found");
      }
    });

    server.listen(HTTP_PORT, () => {
      console.log(`  HTTP server: http://localhost:${HTTP_PORT}`);
      resolve(server);
    });
  });
}

// ── Main ────────────────────────────────────────────────────────
async function main() {
  console.log("=== WebSocket Replication — Browser Test ===");
  console.log(`  WS server binary: ${SERVER_BIN}`);
  console.log(`  WASM dir:         ${WASM_DIR}`);
  console.log(`  WS port:          ${WS_PORT}`);
  console.log(`  HTTP port:        ${HTTP_PORT}`);

  // 1. Start native WS server
  const server = spawn(SERVER_BIN, [String(WS_PORT), dbPath], {
    stdio: ["ignore", "pipe", "inherit"],
  });

  server.on("error", (err) => {
    console.error(`Server failed to start: ${err.message}`);
    cleanup();
    process.exit(1);
  });

  // Wait for READY
  await new Promise((resolve, reject) => {
    let buf = "";
    const onData = (chunk) => {
      buf += chunk;
      if (buf.includes("READY")) {
        server.stdout.removeListener("data", onData);
        resolve();
      }
    };
    server.stdout.on("data", onData);
    setTimeout(() => reject(new Error("Server did not become ready in 10 s")), 10_000);
  });
  console.log("  WS server ready");

  // 2. Start HTTP server
  const httpServer = await startHttpServer();

  // 3. Open browser
  const url = `http://localhost:${HTTP_PORT}/test.html?port=${WS_PORT}`;
  console.log(`\n  Open in browser: ${url}\n`);

  try {
    // Try common Linux browser openers
    const opener = process.platform === "darwin" ? "open"
                 : process.platform === "win32"  ? "start"
                 : "xdg-open";
    execSync(`${opener} "${url}"`, { stdio: "ignore" });
  } catch {
    console.log("  (Could not auto-open browser — open the URL manually)");
  }

  // 4. Wait for Ctrl+C
  console.log("  Press Ctrl+C to stop the servers.\n");

  process.on("SIGINT", () => {
    console.log("\n  Shutting down...");
    server.kill("SIGTERM");
    httpServer.close();
    cleanup();
    process.exit(0);
  });

  // Keep alive
  server.on("close", (code) => {
    console.log(`  WS server exited (code ${code})`);
    console.log("  Server finished — it only handles one connection.");
    console.log("  Restart to run again, or press Ctrl+C to exit.");
  });
}

main().catch((err) => {
  console.error(err);
  cleanup();
  process.exit(1);
});