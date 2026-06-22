#!/usr/bin/env node
/**
 * Orchestrator: WebSocket replication integration test
 *
 * 1. Spawns the native server binary (ReplicatingMapStorage sender)
 * 2. Waits for "READY <port>" on stdout
 * 3. Runs the test client using the Leaves JS API via Node.js
 * 4. Checks exit codes
 *
 * Run:
 *   cd tests/ws_replication && npm install
 *   node run.mjs                         # uses default paths
 *   node run.mjs --server ../../build/ws_replication_server \
 *                --wasm-dir ../../js
 *
 * Requirements:
 *   - Native server built:      cmake --build build -j --target ws_replication_server
 *   - Leaves JS library built:  cmake --build build-wasm -j --target leaves_js_output
 *   - Node.js 18+ with --experimental-wasm-jspi
 */

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, readFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT      = join(__dirname, "..", "..");

// ── Parse args ──────────────────────────────────────────────────
const args = process.argv.slice(2);
function argVal(name, fallback) {
  const idx = args.indexOf(name);
  return idx >= 0 && idx + 1 < args.length ? args[idx + 1] : fallback;
}

const SERVER_BIN  = argVal("--server", join(ROOT, "build", "ws_replication_server"));
const WASM_DIR    = argVal("--wasm-dir", join(ROOT, "js"));
const PORT        = parseInt(argVal("--port", "19876"), 10);

// ── Temp dir for server DB ──────────────────────────────────────
const tmp  = mkdtempSync(join(tmpdir(), "leaves-ws-"));
const dbPath = join(tmp, "src.lvs");

function cleanup() {
  try { rmSync(tmp, { recursive: true, force: true }); } catch {}
}

// ── Main ────────────────────────────────────────────────────────
async function main() {
  console.log("=== WebSocket Replication Integration Test ===");
  console.log(`  node:     ${process.version}`);
  console.log(`  server:   ${SERVER_BIN}`);
  console.log(`  wasm dir: ${WASM_DIR}`);
  console.log(`  port:     ${PORT}`);

  // 0. Pre-flight checks
  if (!existsSync(SERVER_BIN)) {
    console.error(`  ERROR: server binary not found at: ${SERVER_BIN}`);
    console.log("\n=== FAIL ===");
    cleanup();
    process.exit(1);
  }
  if (!existsSync(WASM_DIR)) {
    console.error(`  ERROR: wasm dir not found at: ${WASM_DIR}`);
    console.log("\n=== FAIL ===");
    cleanup();
    process.exit(1);
  }
  const keyFiles = ["leaves.js", "leaves.wasm", "leaves_replication.js"];
  const missing = keyFiles.filter(f => !existsSync(join(WASM_DIR, f)));
  if (missing.length > 0) {
    console.error(`  ERROR: missing WASM files in ${WASM_DIR}: ${missing.join(", ")}`);
    console.log("\n=== FAIL ===");
    cleanup();
    process.exit(1);
  }
  console.log("  pre-flight checks OK");

  // 0a. Quick JSPI capability check
  const { execSync } = await import("node:child_process");
  try {
    const jspiCheck = execSync(
      `${process.execPath} --experimental-wasm-jspi -e "console.log('Suspending' in WebAssembly)"`,
      { encoding: "utf8" }
    ).trim();
    const jspiOk = jspiCheck === "true";
    console.log(`  JSPI available on target Node: ${jspiOk}`);
    if (!jspiOk) {
      console.log("  ⚠ '--experimental-wasm-jspi' flag is accepted but WebAssembly.Suspending is not exposed.");
      console.log("  ⚠ This Emscripten build requires a Node.js version where JSPI is fully enabled.");
      console.log("  ⚠ Try a newer Node.js (23+) or rebuild WASM without JSPI.");
    }
  } catch (e) {
    console.log(`  JSPI check failed: ${e.message}`);
  }

  // 1. Start server
  const server = spawn(SERVER_BIN, [String(PORT), dbPath], {
    stdio: ["ignore", "pipe", "inherit"],
  });

  let serverExit = null;
  server.on("close", (code) => { serverExit = code; });
  server.on("error", (err) => {
    console.error(`Server failed to start: ${err.message}`);
    console.error(`  attempted command: ${SERVER_BIN} ${String(PORT)} ${dbPath}`);
    process.exit(1);
  });

  // 2. Wait for READY line
  let serverReady = false;
  await new Promise((resolve, reject) => {
    let buf = "";
    const onData = (chunk) => {
      buf += chunk;
      if (buf.includes("READY")) {
        server.stdout.removeListener("data", onData);
        serverReady = true;
        resolve();
      }
    };
    server.stdout.on("data", onData);
    setTimeout(() => reject(new Error("Server did not become ready in 10 s")), 10_000);
  });
  console.log("  server ready");

  // 3. Run test client using Node.js with preload for ws + fake-indexeddb
  const preload = join(__dirname, "client_preload.cjs");
  const clientScript = join(__dirname, "client.mjs");
  const clientArgs = [
    "--experimental-wasm-jspi",
    "--require", preload,
    clientScript,
    String(PORT),
    "--wasm-dir", WASM_DIR,
  ];
  console.log(`  client command: ${process.execPath} ${clientArgs.join(" ")}`);
  console.log("  starting client...");

  const clientResult = await new Promise((resolve, reject) => {
    const child = spawn(process.execPath, clientArgs, {
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
  const passed = clientResult.stdout.includes("=== PASS ===");
  const clientOk = clientResult.code === 0 || passed;
  const serverOk = serverExit === 0;

  console.log(`\n  server exit: ${serverExit ?? "killed"}`);
  console.log(`  client exit: ${clientResult.code}`);

  // Diagnose exit code 9 — Node.js bad option
  if (clientResult.code === 9) {
    console.log("  ⚠ Note: exit code 9 means Node.js rejected a command-line flag.");
    console.log("  ⚠ The flag '--experimental-wasm-jspi' requires Node.js 22+.");
    console.log(`  ⚠ Current version: ${process.version}`);
  }

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