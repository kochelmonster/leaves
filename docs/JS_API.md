# Leaves JavaScript API

The JavaScript API exposes Leaves database functionality in browser environments via Emscripten/embind WebAssembly bindings backed by IndexedDB.

## Architecture

```
┌───────────────────────────────────────┐
│             JS Application             │
├───────────────────────────────────────┤
│      const Module = await createModule│
│           (leaves.js WASM)             │
│                   │                    │
│                   ▼                    │
│          Module.LeavesStore            │
│                   │                    │
│                   ▼                    │
│       _BrowserStore                    │
│     (IndexedDB backed)                 │
└───────────────────────────────────────┘
```

The module is loaded by calling the default export from `leaves.js` (an Emscripten WASM factory function). Once loaded, the module provides `LeavesStore` and related classes.

## Getting Started

```javascript
import createModule from './leaves.js';

const Module = await createModule();

// Create storage (IndexedDB database)
const store = await Module.LeavesStore.create('my_storage', 10 * 1024 * 1024);

// Open a database within the storage
const db = await store.open('mydb');

// Create a cursor
const c = db.createCursor();

// Write data
await c.find('key1');
await c.setValue('value1');
await c.commit(false);

// Read data back
await c.find('key1');
console.log('key:', c.key());
console.log('value:', await c.getValue());

// Cleanup
c.delete();
await store.close();
await Module.LeavesStore.deleteStorage('my_storage');
```

## LeavesStore

Created via `Module.LeavesStore.create(name, capacity)`.

```typescript
class LeavesStore {
    // Create a new IndexedDB-backed storage.
    static async create(
        name: string,     // IndexedDB database name
        capacity: number  // Cache capacity in bytes
    ): Promise<LeavesStore>;

    // Open a database within this storage
    async open(name: string): Promise<LeavesDB>;

    // Open a replication-capable database
    async openReplication(name: string): Promise<ReplicationDB>;

    // List all database names in this storage.
    // Returns a C++ std::vector<string> wrapped by embind.
    // Iterate with .size() and .get(index), then .delete().
    listDbs(): VectorString;

    // Export all data to a Uint8Array buffer
    exportToBuffer(): Promise<Uint8Array>;

    // Import data from a buffer
    importFromBuffer(data: string | Uint8Array): Promise<void>;

    // Delete an IndexedDB storage by name. The storage does not need to be open.
    static async deleteStorage(name: string): Promise<void>;

    // Close the storage (flush pending writes)
    close(): Promise<void>;

    // Number of pending IndexedDB write operations (static, global)
    static pendingWrites(): number;

    // Whether browser diagnostics were enabled at build time
    static debugEnabled(): boolean;

    // WASM heap memory API for low-copy pointer workflows
    static malloc(size: number): number;
    static free(ptr: number): void;
    static heapU8Slice(ptr: number, len: number): Uint8Array;
    static copyToHeap(dstPtr: number, bytes: Uint8Array): void;
    static copyFromHeap(srcPtr: number, len: number): Uint8Array;
    static allocCopy(bytes: Uint8Array): number;
}
```

### LeavesStore Memory API

`LeavesStore` exposes a first-class memory API so callers can use pointer-based
cursor writes without depending on raw Emscripten internals.

- `malloc(size)` allocates WASM heap memory and returns a pointer.
- `free(ptr)` releases previously allocated memory.
- `heapU8Slice(ptr, len)` returns a `Uint8Array` view over `[ptr, ptr + len)`.
- `copyToHeap(dstPtr, bytes)` copies bytes into WASM memory.
- `copyFromHeap(srcPtr, len)` copies bytes out of WASM memory.
- `allocCopy(bytes)` allocates and copies in one call.

## LeavesDB

Returned by `store.open(name)`.

```typescript
class LeavesDB {
    // Set aspect callbacks (see Aspect API section)
    setAspectCallbacks(callbacks: object): void;

    // Create a cursor for database operations
    createCursor(): LeavesCursor;
}
```

## LeavesCursor

The primary data access object. Obtained from `db.createCursor()`.

```typescript
class LeavesCursor {
    // Start a transaction on the cursor.
    // nonBlocking: if true, returns false instead of blocking on conflict.
    //
    // ⚠️ The `use_wal` parameter from the C++ TCursor::start_transaction()
    //    is NOT exposed. The embind wrapper hardcodes it to false because
    //    IndexedDB provides its own write durability, making WAL unnecessary.
    async startTransaction(nonBlocking?: boolean): Promise<boolean>;

    // Navigation
    async find(key: string): Promise<void>;
    async first(): Promise<void>;
    async last(): Promise<void>;
    async next(): Promise<void>;
    async prev(): Promise<void>;

    // State
    isValid(): boolean;

    // Read current key (synchronous — reads from cursor position)
    key(): string;
    keyBytes(): Uint8Array;

    // Read/write value at current cursor position
    async getValue(): Promise<string>;
    async setValuePtr(valuePtr: number, valueLen: number): Promise<void>;
    async setValue(value: string): Promise<void>;
    async getValueBytes(): Promise<Uint8Array>;
    async setValueBytes(value: Uint8Array): Promise<void>;

    // Delete the entry at current cursor position
    async remove(): Promise<void>;

    // Check if this cursor currently holds an active write transaction
    // Returns true between a successful startTransaction() and the
    // subsequent commit() or rollback().
    isTransactionActive(): boolean;

    // Commit or rollback pending changes
    async commit(sync?: boolean): Promise<void>;
    async rollback(): Promise<void>;

    // Aspect context (per-cursor JS object)
    aspectContext(): object;
}
```

`setValuePtr(valuePtr, valueLen)` is the low-copy write primitive.
`setValue(value)` and `setValueBytes(value)` are kept as convenience wrappers
that forward to the same underlying pointer-based write path.

### Usage Patterns

**Write entries:**
```javascript
await c.find('key1');
await c.setValue('value1');
await c.find('key2');
await c.setValue('value2');
await c.commit(false);
```

**Read entries:**
```javascript
await c.find('key1');
if (c.isValid()) {
    console.log(c.key(), await c.getValue());
}
```

**Sequential forward iteration:**
```javascript
await c.first();
while (c.isValid()) {
    console.log(c.key());
    await c.next();
}
```

**Reverse iteration:**
```javascript
await c.last();
while (c.isValid()) {
    console.log(c.key());
    await c.prev();
}
```

**Delete an entry:**
```javascript
await c.find('key1');
await c.remove();
await c.commit(false);
```

**Rollback uncommitted changes:**
```javascript
await c.find('temp_key');
await c.setValue('temp_value');
await c.rollback();
```

**Binary key/value:**
```javascript
const binData = new Uint8Array([0, 1, 2, 255, 128]);
await c.find('bin_key');
await c.setValueBytes(binData);
await c.commit(false);

await c.find('bin_key');
const got = new Uint8Array(await c.getValueBytes());
```

**Low-copy write with reusable WASM buffer:**
```javascript
const encoder = new TextEncoder();
const memory = Module.LeavesStore;
let ptr = 0;
let capacity = 0;

function ensureBuffer(needed) {
    if (ptr && capacity >= needed) return;
    if (ptr) memory.free(ptr);
    ptr = memory.malloc(Math.max(1, needed));
    if (!ptr) throw new Error('malloc failed');
    capacity = Math.max(1, needed);
}

const text = 'value1';
const bytes = encoder.encode(text);
ensureBuffer(bytes.byteLength);
memory.copyToHeap(ptr, bytes);

await c.find('key1');
await c.setValuePtr(ptr, bytes.byteLength);
await c.commit(false);

if (ptr) memory.free(ptr);
```

## Aspect API

Aspect callbacks allow interception of read and write operations at the cursor level. All `LeavesStore` databases (both regular and replication) support aspect callbacks natively — no separate import needed.

```javascript
// No separate import needed — aspects are built into LeavesDB
const store = await Module.LeavesStore.create('my_storage', 10 * 1024 * 1024);
const db = await store.open('mydb');

db.setAspectCallbacks({
    initCursorContext: () => ({ writeCount: 0, readCount: 0 }),
    onWrite: (key, value, ctx) => {
        ctx.writeCount++;
        return value; // pass-through or transform
    },
    onRead: (key, data, bigMeta, ctx) => {
        ctx.readCount++;
        return data;
    },
    mayDelete: (key, value, ctx) => true,
    onCommit: (origin, ctx) => {},
});

const cursor = db.createCursor();
await cursor.find('hello');
await cursor.setValue('world');
await cursor.commit(false);
console.log(cursor.aspectContext().writeCount); // per-cursor context
```

```typescript
// Aspects are available directly on LeavesDB returned by store.open():
type AspectCallbacks = {
    initCursorContext?: () => object;
    onWrite?: (key: Uint8Array, value: Uint8Array, ctx: object) => Uint8Array;
    onRead?: (key: Uint8Array, data: Uint8Array, bigMeta: object, ctx: object) => Uint8Array;
    mayDelete?: (key: Uint8Array, value: Uint8Array, ctx: object) => boolean;
    onCommit?: (origin: number, ctx: object) => void;
};

// LeavesDB:
class LeavesDB {
    setAspectCallbacks(callbacks: AspectCallbacks): void;
    createCursor(): LeavesCursor;
}

// LeavesCursor includes:
class LeavesCursor {
    // ... all cursor methods ...
    aspectContext(): object;  // per-cursor JS context object
}
```

## Replication API

Import from `leaves_replication.js`.

```javascript
import { LeavesReplicationSender, LeavesReplicationReceiver } from './leaves_replication.js';
```

```typescript
class LeavesReplicationSender {
    constructor(replicationDB: ReplicationDB, Module: object);
    begin(transport: ReplicationTransport, events: ReplicationEvents): void;
    onMessageReceived(data: string): void;
    state(): 'idle' | 'active' | 'error';
}

class LeavesReplicationReceiver {
    constructor(replicationDB: ReplicationDB, Module: object);
    begin(transport: ReplicationTransport, events: ReplicationEvents): void;
    onMessageReceived(data: string): void;
    state(): 'idle' | 'active' | 'error';
}

type ReplicationTransport = { send: (data: Uint8Array) => void };

type ReplicationEvents = {
    onComplete?: (sessionId: number, nodesTransferred: number) => void;
    onError?: (sessionId: number, message: string) => void;
    onProgress?: (sessionId: number, bytes: number, nodes: number) => void;
};
```

## Restrictions & Limitations

### 1. `use_wal` Not Supported
The `use_wal` option exists in the C++ `TCursor::start_transaction()` but the embind wrapper hardcodes it to `false`. 

### 2. Single-Threaded
Browser JavaScript runs on a single thread. All IndexedDB operations use Emscripten Asyncify for synchronous-style code, which has stack size limitations.

### 3. Asyncify Stack Limitations
Emscripten Asyncify saves and restores the call stack around asynchronous IndexedDB calls. Deeply nested call stacks or large local variables may exceed the default Asyncify stack size.

### 4. IndexedDB Storage Quotas
IndexedDB is subject to browser-specific storage limits (typically ~50% of available disk). Use `navigator.storage.estimate()` to check usage.

### 5. Manual Cleanup
C++ objects allocated by embind must be freed explicitly with `.delete()`:
```javascript
c.delete();           // free cursor
store.close();        // flush and close
Module.LeavesStore.deleteStorage('my_storage'); // delete the entire IndexedDB storage
```

### 6. C++ Vector Iteration
`store.listDbs()` returns a C++ `std::vector<string>`. Use `.size()`, `.get(index)`, and call `.delete()` when done:
```javascript
const dbs = store.listDbs();
for (let i = 0; i < dbs.size(); i++) {
    console.log(dbs.get(i));
}
dbs.delete();
```

## Cross-Reference

- Browser storage internals: `docs/BROWSER_STORAGE.md`
- Replication protocol: `docs/REPLICATION.md`
- Embind bindings: `js/leaves_embind.cpp`
- Aspect API: built into `LeavesDB`/`LeavesCursor` via `setAspectCallbacks()` and `aspectContext()`
- Replication wrappers: `js/leaves_replication.js`
- Test file: `js/test.html`