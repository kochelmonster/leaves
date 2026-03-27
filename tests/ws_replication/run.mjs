#!/usr/bin/env node
/**
 * Orchestrator: WebSocket replication integration test
 *
 * 1. Spawns the native server binary (ReplicatingMapStorage sender)
 * 2. Waits for "READY <port>" on stdout
 * 3. Runs the WASM client (ReplicatingBrowserStorage receiver) via Node.js
 * 4. Checks exit codes
 *
 * Run:
 *   cd tests/ws_replication && npm install
 *   node run.mjs                         # uses default paths
 *   node run.mjs --server ../../build/ws_replication_server \
 *                --client ../../build-wasm/ws_replication_client.js
 */

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
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

const SERVER_BIN  = argVal("--server", join(ROOT, "build", "ws_replication_server"));
const CLIENT_JS   = argVal("--client", join(ROOT, "build-wasm", "ws_replication_client.js"));
const PORT        = parseInt(argVal("--port", "19876"), 10);

// ── Temp dir for server DB ──────────────────────────────────────
const tmp  = mkdtempSync(join(tmpdir(), "leaves-ws-"));
const dbPath = join(tmp, "src.lvs");

function cleanup() {
  try { rmSync(tmp, { recursive: true, force: true }); } catch {}
}

// ── Helpers ─────────────────────────────────────────────────────
function spawnAsync(cmd, args, opts = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(cmd, args, { stdio: ["ignore", "pipe", "inherit"], ...opts });
    let stdout = "";
    child.stdout.on("data", (d) => { stdout += d; });
    child.on("error", reject);
    child.on("close", (code) => resolve({ code, stdout, child }));
    // Expose child for early kill
    resolve.child = child;
  });
}

// ── Main ────────────────────────────────────────────────────────
async function main() {
  console.log("=== WebSocket Replication Integration Test ===");
  console.log(`  server: ${SERVER_BIN}`);
  console.log(`  client: ${CLIENT_JS}`);
  console.log(`  port:   ${PORT}`);

  // 1. Start server
  const server = spawn(SERVER_BIN, [String(PORT), dbPath], {
    stdio: ["ignore", "pipe", "inherit"],
  });

  let serverExit = null;
  server.on("close", (code) => { serverExit = code; });
  server.on("error", (err) => {
    console.error(`Server failed to start: ${err.message}`);
    process.exit(1);
  });

  // 2. Wait for READY line
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
  console.log("  server ready");

  // 3. Run WASM client
  console.log("  starting client...");
  const clientResult = await new Promise((resolve, reject) => {
    const preload = join(__dirname, "client_preload.cjs");
    const child = spawn(process.execPath, [
      "--experimental-wasm-jspi",
      "--require", preload,
      CLIENT_JS,
      String(PORT),
    ], {
      stdio: ["ignore", "pipe", "inherit"],
    });
    let stdout = "";
    child.stdout.on("data", (d) => {
      const s = d.toString();
      stdout += s;
      process.stdout.write("  [client] " + s);
    });
    child.on("error", reject);
    child.on("close", (code) => resolve({ code, stdout }));
  });

  // 4. Wait for server to exit (it should after WS close)
  if (serverExit === null) {
    await new Promise((resolve) => {
      const timeout = setTimeout(() => {
        server.kill("SIGTERM");
        resolve();
      }, 5000);
      server.on("close", () => { clearTimeout(timeout); resolve(); });
    });
  }

  // 5. Report
  const clientOk = clientResult.code === 0 && clientResult.stdout.includes("PASS");
  const serverOk = serverExit === 0;

  console.log(`\n  server exit: ${serverExit ?? "killed"}`);
  console.log(`  client exit: ${clientResult.code}`);

  if (clientOk && serverOk) {
    console.log("\n=== PASS ===");
    cleanup();
    process.exit(0);
  } else {
    console.log("\n=== FAIL ===");
    cleanup();
    process.exit(1);
  }
}

main().catch((err) => {
  console.error(err);
  cleanup();
  process.exit(1);
});
