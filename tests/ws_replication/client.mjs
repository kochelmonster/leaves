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

// ── Diagnostic logger with elapsed time ────────────────────────
const t0 = Date.now();
function dbg(...args) {
  const t = (Date.now() - t0) / 1000;
  console.log(`[${t.toFixed(3)}s]`, ...args);
}

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

async function setValueFromStringWithReserve(Module, cursor, value) {
  const bytes = new TextEncoder().encode(value);
  if (cursor && typeof cursor.reserveBytes === "function") {
    const view = await cursor.reserveBytes(bytes.byteLength);
    if (!view || typeof view.set !== "function") {
      throw new Error("Cursor.reserveBytes returned an invalid Uint8Array view");
    }
    if (view.byteLength < bytes.byteLength) {
      throw new Error("Cursor.reserveBytes returned a short view");
    }
    view.set(bytes);
    return;
  }
  if (!cursor || typeof cursor.reserve !== "function") {
    throw new Error("Cursor.reserve is unavailable");
  }
  const heap = Module?.HEAPU8;
  if (!heap || typeof heap.set !== "function") {
    throw new Error("Module.HEAPU8 is unavailable");
  }
  const ptr = Number(await cursor.reserve(bytes.byteLength));
  if (!ptr) {
    throw new Error("Cursor.reserve returned an invalid pointer");
  }
  heap.set(bytes, ptr);
}

// ── Run test ──────────────────────────────────────────────────
async function runTest() {
  console.log("=== WebSocket Replication Node.js Test ===");
  console.log("");

  try {
    // ── 1. Load WASM module ──────────────────────────────────────
    dbg("Step 1: Loading WASM module...");
    const createModule = (await import(join(WASM_DIR, "leaves.js"))).default;
    const { LeavesReplicationReceiver } = await import(join(WASM_DIR, "leaves_replication.js"));

    dbg("Step 1b: Calling createModule()...");
    const Module = await createModule({
      locateFile: (path) => join(WASM_DIR, path),
    });
    dbg("Step 1c: WASM module loaded");
    console.log("");

    // ── 2. Create storage (LeavesStore + openReplication) ────────
    dbg("Step 2: Creating LeavesStore...");
    const store = await Module.LeavesStore.create("test_ws_repl", 100 * 1024 * 1024);
    dbg("Step 2b: LeavesStore created, typeof store=" + typeof store);

    dbg("Step 2c: Calling store.openReplication('testdb')...");
    const repldb = await store.openReplication("testdb");
    dbg("Step 2d: openReplication returned, typeof repldb=" + typeof repldb);
    console.log("");

    // ── 3. Connect WebSocket to native server ─────────────────────
    const url = "ws://localhost:" + PORT;
    dbg("Step 3: Connecting WebSocket to " + url + "...");
    const ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    dbg("Step 3b: WebSocket object created, readyState=" + ws.readyState);

    // Set up message queues BEFORE awaiting onopen
    let textQueue = [];
    let binQueue = [];

    ws.onmessage = (ev) => {
      const typ = typeof ev.data === "string" ? "text" : "binary";
      dbg("Step 3c: onmessage received (" + typ + "), length=" + (ev.data.length || ev.data.byteLength));
      if (typeof ev.data === "string") {
        textQueue.push(ev.data);
      } else {
        binQueue.push(new Uint8Array(ev.data));
      }
    };

    ws.onclose = () => {
      dbg("Step 3d: WebSocket onclose fired, readyState=" + ws.readyState);
    };
    ws.onerror = (e) => {
      dbg("Step 3e: WebSocket onerror fired, message=" + (e.message || e));
    };

    dbg("Step 3f: Awaiting WebSocket onopen...");
    await new Promise((resolve, reject) => {
      ws.onopen = () => { dbg("Step 3g: WebSocket onopen fired"); resolve(); };
      ws.onerror = (e) => { dbg("Step 3h: WebSocket onerror during connect"); reject(e); };
      setTimeout(() => reject(new Error("WebSocket timeout after 10s")), 10000);
    });
    dbg("Step 3i: WebSocket connected, readyState=" + ws.readyState);
    console.log("");

    // ── 4. Receive replication data via the FSM ───────────────────
    dbg("Step 4: Creating LeavesReplicationReceiver...");
    const receiver = new LeavesReplicationReceiver(repldb, Module);
    dbg("Step 4b: LeavesReplicationReceiver created");

    let completed = false;
    let errored = false;

    dbg("Step 4c: Calling receiver.begin()...");
    await receiver.begin({ send: data => ws.send(data) }, {
      onComplete: (sid, n) => {
        dbg("Step 4d: onComplete called, sid=" + sid + " nodes=" + n);
        completed = true;
      },
      onError: (sid, err) => {
        dbg("Step 4e: onError called, sid=" + sid + " err=" + err);
        errored = true;
      }
    });
    dbg("Step 4f: receiver.begin() returned, state=" + receiver.state());

    dbg("Step 4g: Entering poll loop...");
    let loopCount = 0;
    while (receiver.state() === "active") {
      loopCount++;
      dbg("Step 4h (iteration " + loopCount + "): state=active, binQueue.length=" + binQueue.length + " textQueue.length=" + textQueue.length + " calling nextMessage()...");
      const msg = await nextMessage(binQueue, ws);
      if (msg === null) {
        dbg("Step 4i: nextMessage returned null (socket closed)");
        errored = true;
        break;
      }
      dbg("Step 4j: nextMessage returned msg of length " + msg.length + ", calling onMessageReceived...");
      await receiver.onMessageReceived(msg);
      dbg("Step 4k: onMessageReceived returned, state=" + receiver.state());
    }

    dbg("Step 4l: Exited poll loop (errored=" + errored + " completed=" + completed + " state=" + receiver.state() + ")");

    if (errored) {
      console.log("Replication failed");
    } else {
      dbg("Step 5a: Replication finished, verifying data...");
      console.log("");

      // ── 5. Verify replicated data ───────────────────────────────
      console.log("Verifying replicated data...");

      const c = await repldb.createCursor();

      await c.find("hello");
      check(c.isValid(), "key 'hello' exists");
      if (c.isValid()) checkEq(await c.getValue(), "world", "hello → world");

      await c.find("foo");
      check(c.isValid(), "key 'foo' exists");
      if (c.isValid()) checkEq(await c.getValue(), "bar", "foo → bar");

      await c.find("count");
      check(c.isValid(), "key 'count' exists");
      if (c.isValid()) checkEq(await c.getValue(), "12345", "count → 12345");

      await c.find("ptr_smoke");
      await setValueFromStringWithReserve(Module, c, "smoke_ptr");
      await c.commit(false);

      await c.find("ptr_smoke");
      check(c.isValid(), "key 'ptr_smoke' exists");
      if (c.isValid()) checkEq(await c.getValue(), "smoke_ptr", "ptr_smoke → smoke_ptr");

      c.delete();
    }

    // ── 6. Cleanup ───────────────────────────────────────────────
    dbg("Step 6: Cleaning up (closing ws, closing store)...");
    ws.close();
    await store.close();
    dbg("Step 6b: Cleanup done");

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