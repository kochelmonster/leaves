const ONE_MB = 1048576;
const DEFAULT_STORAGE_PREFIX = 'bench_js';
const DEFAULT_DB_NAME = 'benchmark';
const DEFAULT_DIAG_WARN_MS = 10000;
const DEFAULT_PROGRESS_MS = 3000;

const LEAVES_BENCHMARKS = [
  'fillseq',
  'fillrandom',
  'overwrite',
  'readrandom',
  'readseq',
  'fillrand100K',
  'fillseq100K',
  'readseq100K',
  'readrand100K',
  'fillseqsync',
  'fillrandsync',
];

const IDB_BENCHMARKS = [
  'idb_fillseq',
  'idb_fillrandom',
  'idb_overwrite',
  'idb_readrandom',
  'idb_readseq',
  'idb_fillrand100K',
  'idb_fillseq100K',
  'idb_readseq100K',
  'idb_readrand100K',
  'idb_fillseqsync',
  'idb_fillrandsync',
];

function nowSeconds() {
  return performance.now() / 1000;
}

function nowMs() {
  return performance.now();
}

function formatMeta(meta) {
  if (!meta) {
    return '';
  }
  const parts = Object.entries(meta)
    .filter(([, value]) => value !== undefined && value !== null && value !== '')
    .map(([key, value]) => `${key}=${value}`);
  return parts.length > 0 ? ` (${parts.join(', ')})` : '';
}

function padKey(key) {
  return String(key).padStart(16, '0');
}

class Random {
  constructor(seed) {
    this.seed = seed & 0x7fffffff;
    if (this.seed === 0 || this.seed === 2147483647) this.seed = 1;
  }

  next() {
    const mod = 2147483647;
    const mul = 16807;
    const product = this.seed * mul;
    this.seed = ((product >> 31) + (product & mod)) >>> 0;
    if (this.seed > mod) this.seed -= mod;
    return this.seed;
  }
}

class RandomGenerator {
  constructor() {
    this.pos = 0;
    this.data = new Uint8Array(ONE_MB);
    const rnd = new Random(301);
    for (let i = 0; i < this.data.length; i += 1) {
      this.data[i] = rnd.next() & 0xff;
    }
  }

  generateView(length) {
    if (this.pos + length > this.data.length) {
      this.pos = 0;
    }
    const start = this.pos;
    this.pos += length;
    return this.data.subarray(start, start + length);
  }

  generateOwned(length) {
    return new Uint8Array(this.generateView(length));
  }
}

function openIndexedDb(name) {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(name, 1);
    request.onupgradeneeded = event => {
      const db = event.target.result;
      if (!db.objectStoreNames.contains('kv')) {
        db.createObjectStore('kv');
      }
    };
    request.onsuccess = event => resolve(event.target.result);
    request.onerror = () => reject(request.error || new Error(`Failed to open IndexedDB ${name}`));
  });
}

function txDone(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error || new Error('IndexedDB transaction failed'));
    tx.onabort = () => reject(tx.error || new Error('IndexedDB transaction aborted'));
  });
}

function reqDone(request) {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error || new Error('IndexedDB request failed'));
  });
}

export class BrowserBenchmarkRunner {
  constructor(Module, options) {
    this.Module = Module;
    this.log = options.log ?? console.log;
    this.num = options.num ?? 10000;
    this.reads = options.reads ?? this.num;
    this.valueSize = options.valueSize ?? 100;
    this.batchSize = options.batchSize ?? 1000;
    this.diagEnabled = Boolean(options.diagEnabled);
    this.diagWarnMs = options.diagWarnMs ?? DEFAULT_DIAG_WARN_MS;
    this.progressEveryMs = options.progressEveryMs ?? DEFAULT_PROGRESS_MS;
    this.progressEveryOps = Math.max(1, options.progressEveryOps ?? Math.max(1000, Math.floor(this.num / 10)));
    this.runId = `${Date.now().toString(36)}_${Math.floor(Math.random() * 1e9).toString(36)}`;
    this.leafDbIndex = 0;
    this.idbIndex = 0;
    this.leavesStore = null;
    this.leavesDb = null;
    this.rawDb = null;
    this.rawStorageName = '';
    this.gen = new RandomGenerator();
    this.rand = new Random(301);
    this.start = 0;
    this.bytes = 0;
    this.done = 0;
    this.message = '';
    this.currentBench = '-';
    this.currentPhase = 'idle';
    this.currentAwait = '';
    this.awaitSinceMs = 0;
    this.lastProgressAtMs = 0;
    this.lastProgressOps = 0;
    this.heartbeatTimer = null;
    this.pendingWritesLast = -1;
    this.pendingWritesLastAtMs = 0;
    this.pendingWritesSampleEveryMs = 100;
  }

  printHeader() {
    const keySize = 16;
    this.log('Engine:      Leaves JS Browser Benchmark');
    this.log(`Keys:        ${keySize} bytes each`);
    this.log(`Values:      ${this.valueSize} bytes each`);
    this.log(`Entries:     ${this.num}`);
    this.log(`RawSize:     ${(((keySize + this.valueSize) * this.num) / ONE_MB).toFixed(1)} MB (estimated)`);
    this.log('------------------------------------------------');
    this.diag('Diagnostics enabled', {
      warnMs: this.diagWarnMs,
      progressMs: this.progressEveryMs,
      progressOps: this.progressEveryOps,
    });
  }

  diag(message, meta = undefined) {
    if (!this.diagEnabled) {
      return;
    }
    this.log(`[diag] ${message}${formatMeta(meta)}`);
  }

  setPhase(name, meta = undefined) {
    this.currentPhase = name;
    this.diag(`phase=${name}`, meta);
  }

  pendingWrites(options = undefined) {
    const allowDuringAwait = Boolean(options?.allowDuringAwait);
    if (this.currentAwait && !allowDuringAwait) {
      return this.pendingWritesLast;
    }

    const now = nowMs();
    if (!allowDuringAwait && (now - this.pendingWritesLastAtMs) < this.pendingWritesSampleEveryMs) {
      return this.pendingWritesLast;
    }

    try {
      const fn = this.Module?.LeavesStore?.pendingWrites;
      if (typeof fn === 'function') {
        const value = Number(fn());
        this.pendingWritesLast = Number.isFinite(value) ? value : -1;
        this.pendingWritesLastAtMs = now;
        return this.pendingWritesLast;
      }
    } catch {
      // Keep the last known value to avoid destabilizing diagnostics.
      return this.pendingWritesLast;
    }
    return this.pendingWritesLast;
  }

  heapU8() {
    const heap = this.Module?.HEAPU8;
    if (!heap || typeof heap.set !== 'function') {
      throw new Error('Module.HEAPU8 is unavailable. Rebuild leaves.js with heap exports enabled.');
    }
    return heap;
  }

  startHeartbeat(totalOps = undefined) {
    if (!this.diagEnabled) {
      return;
    }
    this.stopHeartbeat();
    this.lastProgressAtMs = nowMs();
    this.lastProgressOps = 0;
    this.heartbeatTimer = setInterval(() => {
      const progressMeta = {
        bench: this.currentBench,
        phase: this.currentPhase,
        done: this.done,
        bytes: this.bytes,
        pendingWrites: this.pendingWrites(),
        targetOps: totalOps,
      };
      if (this.currentAwait) {
        progressMeta.awaiting = this.currentAwait;
        progressMeta.awaitMs = Math.round(nowMs() - this.awaitSinceMs);
      }
      this.diag('heartbeat', progressMeta);
    }, this.progressEveryMs);
  }

  stopHeartbeat() {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  maybeLogProgress(stage, current, total) {
    if (!this.diagEnabled) {
      return;
    }
    const now = nowMs();
    const opsDelta = this.done - this.lastProgressOps;
    const timeDelta = now - this.lastProgressAtMs;
    if (current === total || opsDelta >= this.progressEveryOps || timeDelta >= this.progressEveryMs) {
      this.lastProgressOps = this.done;
      this.lastProgressAtMs = now;
      this.diag('progress', {
        bench: this.currentBench,
        stage,
        current,
        total,
        done: this.done,
        pendingWrites: this.pendingWrites(),
      });
    }
  }

  async withAwait(label, action, meta = undefined) {
    const started = nowMs();
    this.currentAwait = label;
    this.awaitSinceMs = started;
    try {
      return await action();
    } finally {
      const elapsedMs = Math.round(nowMs() - started);
      if (this.diagEnabled && elapsedMs >= this.diagWarnMs) {
        this.diag('slow-await', {
          label,
          elapsedMs,
          pendingWrites: this.pendingWrites(),
          ...meta,
        });
      }
      this.currentAwait = '';
      this.awaitSinceMs = 0;
    }
  }

  async writeReservedValue(cursor, value) {
    if (cursor && typeof cursor.reserveBytes === 'function') {
      const view = await cursor.reserveBytes(value.byteLength);
      if (!view || typeof view.set !== 'function') {
        throw new Error('Cursor.reserveBytes returned an invalid Uint8Array view.');
      }
      if (view.byteLength < value.byteLength) {
        throw new Error(`Cursor.reserveBytes returned a short view (${view.byteLength} < ${value.byteLength}).`);
      }
      view.set(value);
      return;
    }
    if (!cursor || typeof cursor.reserve !== 'function') {
      throw new Error('Cursor.reserve/reserveBytes is unavailable. Rebuild leaves.js with updated cursor bindings.');
    }
    const ptr = Number(await cursor.reserve(value.byteLength));
    if (!Number.isFinite(ptr) || ptr <= 0) {
      throw new Error(`Cursor.reserve returned invalid pointer (${ptr}) for ${value.byteLength} bytes`);
    }
    this.heapU8().set(value, ptr);
  }

  startRun() {
    this.start = nowSeconds();
    this.bytes = 0;
    this.done = 0;
    this.message = '';
    this.lastProgressAtMs = nowMs();
    this.lastProgressOps = 0;
  }

  finishedSingleOp() {
    this.done += 1;
  }

  stop(name) {
    const elapsed = Math.max(nowSeconds() - this.start, Number.EPSILON);
    const done = Math.max(this.done, 1);
    if (this.bytes > 0) {
      const rate = `${((this.bytes / ONE_MB) / elapsed).toFixed(1)} MB/s`;
      this.message = this.message ? `${rate} ${this.message}` : rate;
    }
    this.log(`${name.padEnd(16)} : ${(elapsed * 1e6 / done).toFixed(3)} micros/op;${this.message ? ` ${this.message}` : ''}`);
    this.diag('benchmark-finished', {
      bench: name,
      elapsedMs: Math.round(elapsed * 1000),
      done,
      pendingWrites: this.pendingWrites(),
    });
  }

  async openFreshLeavesStorage() {
    if (this.leavesStore) {
      await this.withAwait('leavesStore.close', () => this.leavesStore.close(), {
        bench: this.currentBench,
      });
    }
    this.leafDbIndex += 1;
    const storageName = `${DEFAULT_STORAGE_PREFIX}_leaves_${this.runId}_${this.leafDbIndex}`;
    this.diag('opening-fresh-leaves-storage', { storageName });
    this.leavesStore = await this.withAwait('LeavesStore.create',
      () => this.Module.LeavesStore.create(storageName, 10 * 1024 * 1024), { storageName });
    this.leavesDb = await this.withAwait('leavesStore.open',
      () => this.leavesStore.open(DEFAULT_DB_NAME), { dbName: DEFAULT_DB_NAME });
  }

  async ensureLeavesDb() {
    if (!this.leavesDb) {
      throw new Error('Leaves database has not been initialized');
    }
  }

  async writeLeaves(order, fresh, numEntries, valueSize, entriesPerBatch, sync) {
    if (fresh) {
      await this.openFreshLeavesStorage();
      this.startRun();
    }
    await this.ensureLeavesDb();
    if (numEntries !== this.num) {
      this.message = `(${numEntries} ops)`;
    }

    this.setPhase('write-leaves', {
      order,
      fresh,
      numEntries,
      valueSize,
      entriesPerBatch,
      sync,
    });

    const cursor = await this.withAwait('leavesDb.createCursor', () => this.leavesDb.createCursor(), {
      order,
      numEntries,
    });
    for (let i = 0; i < numEntries; i += entriesPerBatch) {
      for (let j = 0; j < entriesPerBatch; j += 1) {
        const keyIndex = order === 'sequential' ? i + j : (this.rand.next() % numEntries);
        const key = padKey(keyIndex);
        const value = this.gen.generateView(valueSize);
        this.bytes += key.length + value.byteLength;
        await this.withAwait('cursor.find', () => cursor.find(key), { op: this.done + 1 });
        await this.withAwait('cursor.reserve', () => this.writeReservedValue(cursor, value), { op: this.done + 1 });
        this.finishedSingleOp();
        this.maybeLogProgress('write-leaves-ops', Math.min(i + j + 1, numEntries), numEntries);
      }
      await this.withAwait('cursor.commit', () => cursor.commit(sync), {
        batchStart: i,
        batchSize: entriesPerBatch,
      });
      this.maybeLogProgress('write-leaves-batches', Math.min(i + entriesPerBatch, numEntries), numEntries);
    }
  }

  async readLeavesSequential(repeats = 1) {
    await this.ensureLeavesDb();
    this.setPhase('read-leaves-sequential', { repeats });
    for (let r = 0; r < repeats; r += 1) {
      const cursor = await this.withAwait('leavesDb.createCursor', () => this.leavesDb.createCursor(), {
        repeat: r + 1,
        repeats,
      });
      await this.withAwait('cursor.first', () => cursor.first(), { repeat: r + 1 });
      let scanned = 0;
      while (cursor.isValid()) {
        const key = cursor.key();
        const value = await this.withAwait('cursor.getValueBytes', () => cursor.getValueBytes(), {
          repeat: r + 1,
        });
        this.bytes += key.length + value.byteLength;
        this.finishedSingleOp();
        scanned += 1;
        this.maybeLogProgress('read-leaves-seq', scanned, this.num);
        await this.withAwait('cursor.next', () => cursor.next(), { repeat: r + 1 });
      }
    }
  }

  async readLeavesRandom(readCount) {
    await this.ensureLeavesDb();
    this.setPhase('read-leaves-random', { readCount });
    const cursor = await this.withAwait('leavesDb.createCursor', () => this.leavesDb.createCursor(), { readCount });
    for (let i = 0; i < readCount; i += 1) {
      const key = padKey(this.rand.next() % readCount);
      await this.withAwait('cursor.find', () => cursor.find(key), { readIndex: i + 1, readCount });
      if (cursor.isValid()) {
        const value = await this.withAwait('cursor.getValueBytes', () => cursor.getValueBytes(), {
          readIndex: i + 1,
          readCount,
        });
        this.bytes += cursor.key().length + value.byteLength;
      }
      this.finishedSingleOp();
      this.maybeLogProgress('read-leaves-rand', i + 1, readCount);
    }
  }

  async openFreshRawStorage() {
    if (this.rawDb) {
      this.rawDb.close();
    }
    this.idbIndex += 1;
    this.rawStorageName = `${DEFAULT_STORAGE_PREFIX}_raw_${this.runId}_${this.idbIndex}`;
    this.diag('opening-fresh-raw-storage', { storageName: this.rawStorageName });
    this.rawDb = await this.withAwait('openIndexedDb', () => openIndexedDb(this.rawStorageName), {
      storageName: this.rawStorageName,
    });
  }

  async ensureRawDb() {
    if (!this.rawDb) {
      throw new Error('Raw IndexedDB has not been initialized');
    }
  }

  async writeRaw(order, fresh, numEntries, valueSize, entriesPerBatch) {
    if (fresh) {
      await this.openFreshRawStorage();
      this.startRun();
    }
    await this.ensureRawDb();
    if (numEntries !== this.num) {
      this.message = `(${numEntries} ops)`;
    }

    this.setPhase('write-raw', {
      order,
      fresh,
      numEntries,
      valueSize,
      entriesPerBatch,
    });

    for (let i = 0; i < numEntries; i += entriesPerBatch) {
      const tx = this.rawDb.transaction('kv', 'readwrite');
      const store = tx.objectStore('kv');
      for (let j = 0; j < entriesPerBatch; j += 1) {
        const keyIndex = order === 'sequential' ? i + j : (this.rand.next() % numEntries);
        const key = padKey(keyIndex);
        const value = this.gen.generateOwned(valueSize);
        this.bytes += key.length + value.byteLength;
        store.put(value, key);
        this.finishedSingleOp();
        this.maybeLogProgress('write-raw-ops', Math.min(i + j + 1, numEntries), numEntries);
      }
      await this.withAwait('txDone', () => txDone(tx), {
        mode: 'readwrite',
        batchStart: i,
        batchSize: entriesPerBatch,
      });
    }
  }

  async readRawSequential(repeats = 1) {
    await this.ensureRawDb();
    this.setPhase('read-raw-sequential', { repeats });
    for (let r = 0; r < repeats; r += 1) {
      const tx = this.rawDb.transaction('kv', 'readonly');
      const store = tx.objectStore('kv');
      for (let i = 0; i < this.num; i += 1) {
        const key = padKey(i);
        const value = await this.withAwait('reqDone(get)', () => reqDone(store.get(key)), {
          mode: 'readonly',
          repeat: r + 1,
          index: i + 1,
        });
        if (value) {
          this.bytes += key.length + value.byteLength;
        }
        this.finishedSingleOp();
        this.maybeLogProgress('read-raw-seq', i + 1, this.num);
      }
      await this.withAwait('txDone', () => txDone(tx), {
        mode: 'readonly',
        repeat: r + 1,
      });
    }
  }

  async readRawRandom(readCount) {
    await this.ensureRawDb();
    this.setPhase('read-raw-random', { readCount });
    const tx = this.rawDb.transaction('kv', 'readonly');
    const store = tx.objectStore('kv');
    for (let i = 0; i < readCount; i += 1) {
      const key = padKey(this.rand.next() % readCount);
      const value = await this.withAwait('reqDone(get)', () => reqDone(store.get(key)), {
        mode: 'readonly',
        readIndex: i + 1,
        readCount,
      });
      if (value) {
        this.bytes += key.length + value.byteLength;
      }
      this.finishedSingleOp();
      this.maybeLogProgress('read-raw-rand', i + 1, readCount);
    }
    await this.withAwait('txDone', () => txDone(tx), {
      mode: 'readonly',
      readCount,
    });
  }

  async runOne(name) {
    this.currentBench = name;
    this.startHeartbeat();
    this.setPhase('benchmark-start', { name });
    this.startRun();
    let known = true;

    try {
      if (name === 'fillseq') {
        await this.writeLeaves('sequential', true, this.num, this.valueSize, this.batchSize, false);
      } else if (name === 'fillrandom') {
        await this.writeLeaves('random', true, this.num, this.valueSize, this.batchSize, false);
      } else if (name === 'overwrite') {
        await this.writeLeaves('random', false, this.num, this.valueSize, this.batchSize, false);
      } else if (name === 'fillseqsync') {
        await this.writeLeaves('sequential', true, Math.max(1, Math.floor(this.num / 100)), this.valueSize, 1, true);
      } else if (name === 'fillrandsync') {
        await this.writeLeaves('random', true, Math.max(1, Math.floor(this.num / 100)), this.valueSize, 1, true);
      } else if (name === 'fillseq100K') {
        await this.writeLeaves('sequential', true, Math.max(1, Math.floor(this.num / 1000)), 100000, 1, false);
      } else if (name === 'fillrand100K') {
        await this.writeLeaves('random', true, Math.max(1, Math.floor(this.num / 1000)), 100000, 1, false);
      } else if (name === 'readseq') {
        await this.readLeavesSequential(1);
      } else if (name === 'readrandom') {
        await this.readLeavesRandom(this.reads);
      } else if (name === 'readseq100K') {
        const repeats = Math.max(1, Math.floor(this.num / 1000));
        await this.readLeavesSequential(repeats);
      } else if (name === 'readrand100K') {
        const readCount = Math.max(100, Math.floor(this.num / 1000));
        await this.readLeavesRandom(readCount);
      } else if (name === 'idb_fillseq') {
        await this.writeRaw('sequential', true, this.num, this.valueSize, this.batchSize);
      } else if (name === 'idb_fillrandom') {
        await this.writeRaw('random', true, this.num, this.valueSize, this.batchSize);
      } else if (name === 'idb_overwrite') {
        await this.writeRaw('random', false, this.num, this.valueSize, this.batchSize);
      } else if (name === 'idb_fillseqsync') {
        await this.writeRaw('sequential', true, Math.max(1, Math.floor(this.num / 100)), this.valueSize, 1);
      } else if (name === 'idb_fillrandsync') {
        await this.writeRaw('random', true, Math.max(1, Math.floor(this.num / 100)), this.valueSize, 1);
      } else if (name === 'idb_fillseq100K') {
        await this.writeRaw('sequential', true, Math.max(1, Math.floor(this.num / 1000)), 100000, 1);
      } else if (name === 'idb_fillrand100K') {
        await this.writeRaw('random', true, Math.max(1, Math.floor(this.num / 1000)), 100000, 1);
      } else if (name === 'idb_readseq') {
        await this.readRawSequential(1);
      } else if (name === 'idb_readrandom') {
        await this.readRawRandom(this.reads);
      } else if (name === 'idb_readseq100K') {
        const repeats = Math.max(1, Math.floor(this.num / 1000));
        await this.readRawSequential(repeats);
      } else if (name === 'idb_readrand100K') {
        const readCount = Math.max(100, Math.floor(this.num / 1000));
        await this.readRawRandom(readCount);
      } else {
        known = false;
        if (name) {
          throw new Error(`unknown benchmark '${name}'`);
        }
      }
    } finally {
      this.stopHeartbeat();
      this.currentAwait = '';
      this.awaitSinceMs = 0;
    }

    if (known) {
      this.stop(name);
    }
  }

  async run(which) {
    this.printHeader();
    const selected = which === 'leaves' ? LEAVES_BENCHMARKS
      : which === 'idb' ? IDB_BENCHMARKS
      : [...LEAVES_BENCHMARKS, ...IDB_BENCHMARKS];

    this.diag('selected-benchmarks', {
      which,
      count: selected.length,
      list: selected.join(','),
    });

    for (const name of selected) {
      await this.runOne(name);
    }

    if (this.leavesStore) {
      this.setPhase('teardown-leaves');
      await this.withAwait('leavesStore.close', () => this.leavesStore.close(), {
        storage: 'leavesStore',
      });
    }
    if (this.rawDb) {
      this.setPhase('teardown-raw');
      this.rawDb.close();
    }
    this.setPhase('done');
  }
}

export async function runBrowserBenchmarks(Module, options) {
  const runner = new BrowserBenchmarkRunner(Module, options);
  await runner.run(options.which ?? 'all');
}

export { LEAVES_BENCHMARKS, IDB_BENCHMARKS };