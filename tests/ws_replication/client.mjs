#!/usr/bin/env node
/**
 * WebSocket replication test client (Node.js)
 *
 * Uses the modern Leaves JS API (leaves.js + leaves_replication.js).
 * Run via run.mjs which spawns this as a child process with
 * a preload that mocks WebSocket/IndexedDB.
 *
 * Usage: node client.mjs <port> [--wasm-dir <dir>]
 */

import { WebSocket } from "ws";
import "fake-indexeddb/auto";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, "..", "..");

// ── Parse args ──────────────────────────────────────────────────
const args = process.argv.slice(2);
const PORT = args[0] || "19876";

const wasmDirIdx = args.indexOf("--wasm-dir");
const WASM_DIR = wasmDirIdx >= 0 ? args[wasmDirIdx + 1] : join(ROOT, "js");

// ── Test state ──────────────────────────────────────────────────
let failures = 0;

function check(cond, msg) {
  if (!cond) {
    console.log(`  FAIL: ${msg}`);
    failures++;
  } else {
    console.log(`  PASS: ${msg}`);
  }
}

function checkEq(a, b, msg) {
  if (a !== b) {
    console.log(`  FAIL: ${msg} — got '${a}', expected '${b}'`);
    failures++;
  } else {
    console.log(`  PASS: ${msg}`);
  }
}

// ── Run test ──────────────────────────────────────────────────
async function runTest() {
  console.log("=== WebSocket Replication Node.js Test ===");
  console.log("");

  try {
    // 1. Load WASM module — we need to handle locateFile for Node.js
    console.log("Loading WASM module...");
    const createModule = (await import(join(WASM_DIR, "leaves.js"))).default;
    const { LeavesReplicationReceiver } = await import(join(WASM_DIR, "leaves_replication.js"));

    const Module = await createModule({
      locateFile: (path) => join(WASM_DIR, path),
    });
    console.log("WASM module loaded");
    console.log("");

    // 2. Create storage
    console.log("Creating storage...");
    const store = await Module.LeavesStore.create("test_ws_repl", 100 * 1024 * 1024);
    const repldb = await store.openReplication("testdb");
    console.log("Storage created");
    console.log("");

    // 3. Connect WebSocket to native server
    const url = "ws://localhost:" + PORT;
    console.log(`Connecting to ${url}...`);
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";

    await new Promise((resolve, reject) => {
      ws.onopen = resolve;
      ws.onerror = reject;
      setTimeout(() => reject(new Error("WebSocket timeout")), 10000);
    });
    console.log("Connected");
    console.log("");

    // 4. Set up message queues
    let textQueue = [];
    let binQueue = [];

    ws.onmessage = (ev) => {
      if (typeof ev.data === "string") {
        textQueue.push(ev.data);
      } else {
        binQueue.push(new Uint8Array(ev.data));
      }
    };

    ws.onclose = () => {};
    ws.onerror = (e) => console.log("WebSocket error:", e);

    // 5. Receive replication data
    console.log("Receiving replication...");

    const receiver = new LeavesReplicationReceiver(repldb, Module);

    let completed = false;
    let errored = false;

    receiver.begin({ send: data => ws.send(data) }, {
      onComplete: (sid, n) => {
        console.log(`  Replication complete: ${n} nodes`);
        completed = true;
      },
      onError: (sid, err) => {
        console.log(`  Replication error: ${err}`);
        errored = true;
      }
    });

    while (receiver.state() === "active") {
      const msg = await nextMessage(binQueue, ws);
      if (msg === null) {
        console.log("WebSocket closed during receive");
        errored = true;
        break;
      }
      receiver.onMessageReceived(msg);
    }

    if (errored) {
      console.log("Replication failed");
    } else {
      console.log("Replication finished");
      console.log("");

      // 6. Verify replicated data
      console.log("Verifying replicated data...");

      const c = repldb.createCursor();

      await c.find("hello");
      check(c.isValid(), "key 'hello' exists");
      if (c.isValid()) checkEq(c.getValue(), "world", "hello → world");

      await c.find("foo");
      check(c.isValid(), "key 'foo' exists");
      if (c.isValid()) checkEq(c.getValue(), "bar", "foo → bar");

      await c.find("count");
      check(c.isValid(), "key 'count' exists");
      if (c.isValid()) checkEq(c.getValue(), "12345", "count → 12345");

      c.delete();
    }

    // 7. Cleanup
    ws.close();
    await store.close();

    console.log("");
    if (failures === 0) {
      console.log("=== PASS ===");
      process.exit(0);
    } else {
      console.log(`=== FAIL (${failures} check(s) failed) ===`);
      process.exit(1);
    }

  } catch (e) {
    console.error("Exception:", e.message || e);
    console.log("=== FAIL ===");
    process.exit(1);
  }
}

// ── Message queue helper ──────────────────────────────────────
function nextMessage(queue, ws) {
  if (queue.length > 0) return Promise.resolve(queue.shift());
  return new Promise(resolve => {
    const check = () => {
      if (queue.length > 0) resolve(queue.shift());
      else if (!ws || ws.readyState !== WebSocket.OPEN) resolve(null);
      else setTimeout(check, 10);
    };
    check();
  });
}

// ── Start ─────────────────────────────────────────────────────
runTest();