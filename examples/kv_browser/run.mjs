#!/usr/bin/env node
/**
 * KV Browser Demo — runner script
 *
 * 1. Starts the native kv_demo_server (ReplicatingMapStorage + multi-client WS)
 * 2. Serves index.html + WASM artifacts via HTTP with COOP/COEP headers
 * 3. Opens the browser
 *
 * Usage:
 *   node run.mjs
 *   node run.mjs --ws-port 19876 --http-port 8080
 *
 * Requirements:
 *   - cmake --build build -j --target kv_demo_server
 *   - The `leaves` JS API must be built (see `js/README.md`)
 *   - Chrome 128+ (or any browser with JSPI support)
 */

import { spawn, execSync } from "node:child_process";
import { createServer } from "node:http";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";
import { fileURLToPath } from "node:url";
import { dirname } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname);

// ── Parse args ──────────────────────────────────────────────────
const args = process.argv.slice(2);
function argVal(name, fallback) {
  const idx = args.indexOf(name);
  return idx >= 0 && idx + 1 < args.length ? args[idx + 1] : fallback;
}

const SERVER_BIN = argVal("--server", join(ROOT, "build", "kv_demo_server"));
const WASM_DIR   = argVal("--wasm-dir", join(ROOT, "..", "..", "js"));
const WS_PORT    = parseInt(argVal("--ws-port", "19876"), 10);
const HTTP_PORT  = parseInt(argVal("--http-port", "8080"), 10);

// ── Temp dir for server DB ──────────────────────────────────────
const tmp = mkdtempSync(join(tmpdir(), "leaves-kv-demo-"));
const dbPath = join(tmp, "demo.lvs");

function cleanup() {
  try { rmSync(tmp, { recursive: true, force: true }); } catch {}
}

// ── MIME types ──────────────────────────────────────────────────
const MIME = {
  ".html": "text/html",
  ".js":   "application/javascript",
  ".wasm": "application/wasm",
  ".css":  "text/css",
  ".json": "application/json",
};

// ── Static file server ─────────────────────────────────────────
function startHttpServer() {
  return new Promise((resolve) => {
    const server = createServer((req, res) => {
      const url = new URL(req.url, `http://localhost:${HTTP_PORT}`);
      let filePath;

      // Serve index.html from this directory
      if (url.pathname === "/" || url.pathname === "/index.html") {
        filePath = join(__dirname, "index.html");
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
      console.log("  serving", url.pathname, "->", filePath);

      try {
        const data = readFileSync(filePath);
        const ext = extname(filePath);
        res.writeHead(200, {
          "Content-Type": MIME[ext] || "application/octet-stream",
          // Required for SharedArrayBuffer (JSPI needs these)
          "Cross-Origin-Opener-Policy": "same-origin",
          "Cross-Origin-Embedder-Policy": "require-corp",
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
  console.log("=== Leaves KV Browser Demo ===");
  console.log(`  Server binary: ${SERVER_BIN}`);
  console.log(`  WASM dir:      ${WASM_DIR}`);
  console.log(`  WS port:       ${WS_PORT}`);
  console.log(`  HTTP port:     ${HTTP_PORT}`);
  console.log(`  DB path:       ${dbPath}`);
  console.log();

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
    setTimeout(
      () => reject(new Error("Server did not become ready in 10 s")),
      10_000
    );
  });
  console.log("  WS server ready\n");

  // 2. Start HTTP server
  const httpServer = await startHttpServer();

  // 3. Open browser
  const url = `http://localhost:${HTTP_PORT}/?port=${WS_PORT}`;
  console.log(`  Open in browser: ${url}`);
  console.log(`  Open a second tab at the same URL for live sync demo\n`);
/*
  try {
    const opener =
      process.platform === "darwin"
        ? "open"
        : process.platform === "win32"
          ? "start"
          : "xdg-open";
    execSync(`${opener} "${url}"`, { stdio: "ignore" });
  } catch {
    console.log("  (Could not auto-open browser — open the URL manually)");
  }
*/
  // 4. Keep running until Ctrl+C
  console.log("  Press Ctrl+C to stop.\n");

  process.on("SIGINT", () => {
    console.log("\n  Shutting down...");
    server.kill("SIGTERM");
    httpServer.close();
    cleanup();
    process.exit(0);
  });

  server.on("close", (code) => {
    console.log(`  WS server exited (code ${code})`);
  });
}

main().catch((err) => {
  console.error(err);
  cleanup();
  process.exit(1);
});
