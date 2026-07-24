# Leaves JavaScript API

The JavaScript API exposes Leaves database functionality in browser environments through Emscripten/embind bindings over IndexedDB-backed storage.

The JS API follows the C++ cursor-first model (storage -> database -> cursor), with browser-specific behavior for async I/O.

Important scope note:
- JavaScript exposes regular DB and replication DB APIs.


## Async Model (JSPI vs ASYNCIFY)

The wasm build supports two async backends:
- `JSPI` (default): many methods return Promises.
- `ASYNCIFY`: methods may be synchronous (blocking style from JS perspective).

Use `await` consistently in application code. It works for both backends.

## Getting Started

```javascript
import createModule from './leaves.js';

const Module = await createModule();

// Create storage (IndexedDB database)
const store = await Module.LeavesStore.create('my_storage', 10 * 1024 * 1024);

// Open a database inside the storage
const db = await store.open('main');

// Create a cursor
const c = db.createCursor();

// Write
await c.find('hello');
await c.setValue('world');
await c.commit(false);

// Read
await c.find('hello');
if (c.isValid()) {
    console.log(c.key(), await c.getValue());
}

c.delete();
await store.close();
await Module.LeavesStore.deleteStorage('my_storage');
```

## API Overview

### LeavesStore

Created via `Module.LeavesStore.create(name, capacity)`.

```typescript
class LeavesStore {
    static create(name: string, capacity: number): Promise<LeavesStore>;

    open(name: string): Promise<LeavesDB>;
    openReplication(name: string): Promise<ReplicationDB>;

    // C++ std::vector<string> wrapper from embind.
    // Iterate with .size() and .get(i), then call .delete().
    listDbs(): VectorString;

    // Export current storage content as a copied Uint8Array.
    exportToBuffer(): Uint8Array;

    // Import raw bytes into storage.
    importFromBuffer(data: string | Uint8Array): Promise<void>;

    // Flush/close storage
    close(): Promise<void>;

    static deleteStorage(name: string): Promise<void>;

    static pendingWrites(): number;
    static debugEnabled(): boolean;
}
```

### LeavesDB

Returned by `store.open(name)`.

```typescript
class LeavesDB {
    setAspectCallbacks(callbacks: AspectCallbacks): void;
    createCursor(): LeavesCursor;
}
```

### ReplicationDB

Returned by `store.openReplication(name)`.

```typescript
class ReplicationDB {
    setAspectCallbacks(callbacks: AspectCallbacks): void;
    createCursor(): LeavesReplicationDBCursor;
}
```

### LeavesCursor

Returned by `db.createCursor()`.

```typescript
class LeavesCursor {
    startTransaction(nonBlocking?: boolean): Promise<boolean>;

    find(key: string): Promise<void>;
    first(): Promise<void>;
    last(): Promise<void>;
    next(): Promise<void>;
    prev(): Promise<void>;

    isValid(): boolean;

    key(): string;
    keyBytes(): Uint8Array;

    getValue(): Promise<string>;
    getValueBytes(): Promise<Uint8Array>;

    setValue(value: string): Promise<void>;
    setValueBytes(value: Uint8Array): Promise<void>;

    // reserve returns a wasm heap pointer (number)
    reserve(size: number): Promise<number>;

    // reserveBytes returns a writable Uint8Array view into wasm memory
    reserveBytes(size: number): Promise<Uint8Array>;

    remove(): Promise<void>;

    commit(sync?: boolean): Promise<boolean>;
    rollback(): Promise<boolean>;

    isTransactionActive(): boolean;

    // Refresh cursor view after out-of-band mutation
    update(): void;

    // Per-cursor JS context from aspect system
    aspectContext(): object;
}
```

### LeavesReplicationDBCursor

Returned by `replicationDb.createCursor()`.

Its API mirrors `LeavesCursor`.

```typescript
class LeavesReplicationDBCursor {
    startTransaction(nonBlocking?: boolean): Promise<boolean>;
    find(key: string): Promise<void>;
    first(): Promise<void>;
    last(): Promise<void>;
    next(): Promise<void>;
    prev(): Promise<void>;
    isValid(): boolean;
    key(): string;
    keyBytes(): Uint8Array;
    getValue(): Promise<string>;
    getValueBytes(): Promise<Uint8Array>;
    setValue(value: string): Promise<void>;
    setValueBytes(value: Uint8Array): Promise<void>;
    reserve(size: number): Promise<number>;
    reserveBytes(size: number): Promise<Uint8Array>;
    remove(): Promise<void>;
    commit(sync?: boolean): Promise<boolean>;
    rollback(): Promise<boolean>;
    isTransactionActive(): boolean;
    update(): void;
    aspectContext(): object;
}
```

## Notes on C++ Parity

The JS bindings intentionally differ from full C++ surface in a few places:
- No ConfluenceDB binding in JS.
- `prepare_commit()` is not exposed on JS cursors.
- Cursor `startTransaction()` does not expose C++ `use_wal`; JS path always uses browser storage semantics.

## Usage Patterns

### Write entries

```javascript
await c.find('key1');
await c.setValue('value1');
await c.find('key2');
await c.setValue('value2');
await c.commit(false);
```

### Read entries

```javascript
await c.find('key1');
if (c.isValid()) {
    console.log(c.key(), await c.getValue());
}
```

### Forward iteration

```javascript
await c.first();
while (c.isValid()) {
    console.log(c.key());
    await c.next();
}
```

### Reverse iteration

```javascript
await c.last();
while (c.isValid()) {
    console.log(c.key());
    await c.prev();
}
```

### Binary value I/O

```javascript
const payload = new Uint8Array([0, 1, 2, 255]);
await c.find('bin');
await c.setValueBytes(payload);
await c.commit(false);

await c.find('bin');
const got = new Uint8Array(await c.getValueBytes());
```

### Low-copy write with reserveBytes

```javascript
const bytes = new TextEncoder().encode('hello');
await c.find('k');
const view = await c.reserveBytes(bytes.length);
view.set(bytes);
await c.commit(false);
```

### Pointer-level fallback with reserve

```javascript
const bytes = new TextEncoder().encode('hello');
const ptr = await c.reserve(bytes.length);
Module.HEAPU8.set(bytes, ptr);
await c.commit(false);
```

## Aspect API

Aspect callbacks are available on both `LeavesDB` and `ReplicationDB` via `setAspectCallbacks(...)`.

```javascript
db.setAspectCallbacks({
    initCursorContext: () => ({ writes: 0, reads: 0 }),
    onWrite: (key, value, ctx) => {
        ctx.writes++;
        return value;
    },
    onRead: (key, data, bigMeta, ctx) => {
        ctx.reads++;
        return data;
    },
    mayDelete: (key, value, ctx) => true,
    onCommit: (origin, ctx) => {}
});
```

```typescript
type AspectCallbacks = {
    initCursorContext?: () => object;
    onWrite?: (key: Uint8Array, value: Uint8Array, ctx: object) => Uint8Array;
    onRead?: (key: Uint8Array, data: Uint8Array, bigMeta: object, ctx: object) => Uint8Array;
    mayDelete?: (key: Uint8Array, value: Uint8Array, ctx: object) => boolean;
    onCommit?: (origin: number, ctx: object) => void;
};
```

## Replication Wrappers (JS)

Import wrappers from `js/leaves_replication.js`.

```javascript
import { LeavesReplicationSender, LeavesReplicationReceiver } from './leaves_replication.js';
```

Lifetime note:
- `LeavesReplicationSender` and `LeavesReplicationReceiver` hold embind/native resources.
- Delete both wrappers before deleting `ReplicationDB` and before closing/deleting `LeavesStore`.

Recommended teardown order:

```javascript
receiver.delete();
sender.delete();
replDb.delete();
await store.close();
store.delete();
```

```typescript
class LeavesReplicationSender {
    constructor(replicationDB: ReplicationDB, Module: object);
    begin(transport: ReplicationTransport, events: ReplicationEvents): Promise<void>;
    onMessageReceived(data: string | Uint8Array): Promise<void>;
    state(): 'idle' | 'active' | 'error';
    delete(): void;
}

class LeavesReplicationReceiver {
    constructor(replicationDB: ReplicationDB, Module: object);
    begin(transport: ReplicationTransport, events: ReplicationEvents): Promise<void>;
    onMessageReceived(data: string | Uint8Array): Promise<void>;
    state(): 'idle' | 'active' | 'error';
    delete(): void;
}

type ReplicationTransport = { send: (data: Uint8Array) => void };

type ReplicationEvents = {
    onComplete?: (sessionId: number, nodesTransferred: number) => void;
    onError?: (sessionId: number, message: string) => void;
    onProgress?: (sessionId: number, bytesTransferred: number, nodesTransferred: number) => void;
};
```

## Build and Run (WASM Artifacts, Tests, Benchmark, Example)

All commands below are from repository root unless noted.

### 1) Build wasm artifacts (`js/leaves.js`, `js/leaves.wasm`)

```bash
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j4 --target leaves_js_output
```

This updates:
- `js/leaves.js`
- `js/leaves.wasm`

### 2) Optional debug browser build

Enable browser diagnostics/debug flags:

```bash
emcmake cmake -B build-wasm-debug -G Ninja -DLEAVES_BROWSER_DEBUG=ON
cmake --build build-wasm-debug -j4 --target browser_test
```

### 3) Build variant with ASYNCIFY backend (optional)

Default backend is `JSPI`. To build with `ASYNCIFY`:

```bash
emcmake cmake -B build-wasm-asyncify -G Ninja -DLEAVES_ASYNC_BACKEND=ASYNCIFY
cmake --build build-wasm-asyncify -j4 --target leaves_js_output
```

### 4) Run JavaScript API browser tests

`js/test.html` expects `leaves.js` in the same directory.

```bash
python3 -m http.server -d js 8000
```

Open:

```text
http://localhost:8000/test.html
```

### 4a) Run `ws_replication` integration test (WebSocket)

This test validates end-to-end replication over WebSocket between:
- native sender (`tests/ws_replication/server.cpp`), and
- JS receiver (`tests/ws_replication/client.mjs` or `tests/ws_replication/test.html`).

What it checks:
- receiver reaches completion/idle state,
- replicated keys exist with expected values (`hello -> world`, `foo -> bar`, `count -> 12345`),
- cursor reserve path works (`reserveBytes`/`reserve` smoke check),
- cleanup drains pending writes.

Node.js runner (fully automated):

```bash
cmake --build build -j4 --target ws_replication_server
cmake --build build-wasm -j4 --target leaves_js_output
cd tests/ws_replication
npm install
node run.mjs --server ../../build/ws_replication_server --wasm-dir ../../js
```

Browser runner (opens test page wired to native WS server):

```bash
cmake --build build -j4 --target ws_replication_server
cmake --build build-wasm -j4 --target leaves_js_output
cd tests/ws_replication
npm install
node run_browser.mjs --server ../../build/ws_replication_server --wasm-dir ../../js
```

When running via browser runner, open URL shown by the script (typically `http://localhost:8080/test.html?port=19876`).

### 5) Run browser benchmark

Build artifacts, then serve repository root:

```bash
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j4 --target leaves_js_output
python3 -m http.server 8000
```

Open:

```text
http://localhost:8000/benchmarks/bench.html
```

### 6) Build and run browser example (`examples/kv_browser`)

The example consumes `js/leaves.js` + `js/leaves.wasm`, and builds a native websocket server.

```bash
# Ensure wasm artifacts exist first
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j4 --target leaves_js_output

# Build example server
cd examples/kv_browser
cmake -B build -G Ninja
cmake --build build -j4 --target kv_demo_server

# Run full demo (starts ws server + http server)
node run.mjs
```

## Restrictions and Limitations

1. Browser environment only
- This API is intended for Emscripten/browser use with IndexedDB-backed `_BrowserStore`.

2. Manual lifetime management for embind objects
- Call `.delete()` on embind-allocated objects such as cursors and vector wrappers.
- Replication wrappers (`LeavesReplicationSender`, `LeavesReplicationReceiver`)
    must be deleted before `ReplicationDB`/`LeavesStore` teardown.
- For `LeavesStore`, use `await store.close()` to flush/close storage resources,
  then call `store.delete()` to release the embind handle.

3. IndexedDB quota and browser policy limits
- Storage size and behavior depend on browser quota/persistence policy.

4. WASM memory view lifetime
- Buffers returned by `reserveBytes()` are direct wasm-memory views and can become invalid after memory growth or other mutations.

5. `listDbs()` vector ownership
- `listDbs()` returns an embind-wrapped C++ vector; iterate via `.size()` and `.get(i)`, then call `.delete()`.

## Cross-Reference

- C++ API: `docs/cpp-api.md`
- Browser storage internals: `docs/BROWSER_STORAGE.md`
- Replication docs: `docs/replication/replication.md`
- Embind bindings: `js/leaves_embind.cpp`
- Replication JS wrappers: `js/leaves_replication.js`
- Browser API test page: `js/test.html`
- Browser benchmark: `benchmarks/bench.html`
