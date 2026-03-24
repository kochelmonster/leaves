/**
 * Leaves Browser Storage - JavaScript Helper Module
 *
 * This module provides JavaScript-side support for the _BrowserStore
 * WebAssembly implementation. It handles advanced IndexedDB operations
 * and provides utilities for data import/export.
 *
 * Usage:
 *   import { LeavesStorage } from './leaves_browser_storage.js';
 *   const storage = new LeavesStorage('my_database');
 *   await storage.initialize();
 */

const LEAVES_DB_VERSION = 1;
const LEAVES_STORE_NAME = 'leaves_data';
const LEAVES_META_STORE = 'leaves_meta';

/**
 * LeavesStorage: JavaScript helper for browser-based Leaves database
 */
export class LeavesStorage {
  constructor(dbName) {
    this.dbName = dbName;
    this.db = null;
    this._initPromise = null;
  }

  /**
   * Initialize the IndexedDB connection
   */
  async initialize() {
    if (this._initPromise) {
      return this._initPromise;
    }

    this._initPromise = new Promise((resolve, reject) => {
      const request = indexedDB.open(this.dbName, LEAVES_DB_VERSION);

      request.onerror = () => {
        reject(new Error(`Failed to open IndexedDB: ${request.error}`));
      };

      request.onsuccess = () => {
        this.db = request.result;
        resolve(this);
      };

      request.onupgradeneeded = (event) => {
        const db = event.target.result;

        // Main data store - keyed by string offset
        if (!db.objectStoreNames.contains(LEAVES_STORE_NAME)) {
          db.createObjectStore(LEAVES_STORE_NAME);
        }

        // Metadata store for header and configuration
        if (!db.objectStoreNames.contains(LEAVES_META_STORE)) {
          db.createObjectStore(LEAVES_META_STORE);
        }
      };
    });

    return this._initPromise;
  }

  /**
   * Close the database connection
   */
  close() {
    if (this.db) {
      this.db.close();
      this.db = null;
      this._initPromise = null;
    }
  }

  /**
   * Store a data block at the given offset
   * @param {number} offset - Storage offset
   * @param {ArrayBuffer|Uint8Array} data - Data to store
   */
  async store(offset, data) {
    await this.initialize();

    return new Promise((resolve, reject) => {
      const key = offset === 0 ? 'header' : `area_${offset}`;
      const tx = this.db.transaction(LEAVES_STORE_NAME, 'readwrite');
      const store = tx.objectStore(LEAVES_STORE_NAME);

      // Convert to ArrayBuffer if needed
      const buffer = data instanceof ArrayBuffer ? data : data.buffer.slice(
        data.byteOffset,
        data.byteOffset + data.byteLength
      );

      const request = store.put(buffer, key);

      request.onsuccess = () => resolve();
      request.onerror = () => reject(new Error(`Store failed: ${request.error}`));
    });
  }

  /**
   * Load a data block from the given offset
   * @param {number} offset - Storage offset
   * @returns {Promise<ArrayBuffer|null>} - Retrieved data or null if not found
   */
  async load(offset) {
    await this.initialize();

    return new Promise((resolve, reject) => {
      const key = offset === 0 ? 'header' : `area_${offset}`;
      const tx = this.db.transaction(LEAVES_STORE_NAME, 'readonly');
      const store = tx.objectStore(LEAVES_STORE_NAME);

      const request = store.get(key);

      request.onsuccess = () => {
        resolve(request.result || null);
      };
      request.onerror = () => reject(new Error(`Load failed: ${request.error}`));
    });
  }

  /**
   * Delete a data block at the given offset
   * @param {number} offset - Storage offset
   */
  async delete(offset) {
    await this.initialize();

    return new Promise((resolve, reject) => {
      const key = offset === 0 ? 'header' : `area_${offset}`;
      const tx = this.db.transaction(LEAVES_STORE_NAME, 'readwrite');
      const store = tx.objectStore(LEAVES_STORE_NAME);

      const request = store.delete(key);

      request.onsuccess = () => resolve();
      request.onerror = () => reject(new Error(`Delete failed: ${request.error}`));
    });
  }

  /**
   * Get all stored keys (area offsets)
   * @returns {Promise<string[]>} - Array of keys
   */
  async getAllKeys() {
    await this.initialize();

    return new Promise((resolve, reject) => {
      const tx = this.db.transaction(LEAVES_STORE_NAME, 'readonly');
      const store = tx.objectStore(LEAVES_STORE_NAME);

      const request = store.getAllKeys();

      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(new Error(`GetAllKeys failed: ${request.error}`));
    });
  }

  /**
   * Export entire database to a single ArrayBuffer
   * Format: [header_size(4 bytes)][header][area_count(4 bytes)][entries...]
   * Each entry: [offset(8 bytes)][size(4 bytes)][data]
   *
   * @returns {Promise<ArrayBuffer>} - Serialized database
   */
  async exportToBuffer() {
    await this.initialize();

    const keys = await this.getAllKeys();
    const entries = [];
    let totalSize = 8; // header_size + area_count

    // Collect all data
    for (const key of keys) {
      const data = await this.load(key === 'header' ? 0 : parseInt(key.split('_')[1]));
      if (data) {
        const offset = key === 'header' ? 0 : parseInt(key.split('_')[1]);
        entries.push({ offset, data });
        totalSize += 12 + data.byteLength; // offset(8) + size(4) + data
      }
    }

    // Create output buffer
    const buffer = new ArrayBuffer(totalSize);
    const view = new DataView(buffer);
    const bytes = new Uint8Array(buffer);
    let pos = 0;

    // Write header size placeholder (will be filled by first entry if header exists)
    const headerEntry = entries.find(e => e.offset === 0);
    view.setUint32(pos, headerEntry ? headerEntry.data.byteLength : 0, true);
    pos += 4;

    // Write area count
    view.setUint32(pos, entries.length, true);
    pos += 4;

    // Write each entry
    for (const entry of entries) {
      // Write offset (8 bytes, little-endian as two 32-bit values)
      view.setUint32(pos, entry.offset & 0xFFFFFFFF, true);
      view.setUint32(pos + 4, Math.floor(entry.offset / 0x100000000), true);
      pos += 8;

      // Write size
      view.setUint32(pos, entry.data.byteLength, true);
      pos += 4;

      // Write data
      bytes.set(new Uint8Array(entry.data), pos);
      pos += entry.data.byteLength;
    }

    return buffer;
  }

  /**
   * Import database from ArrayBuffer
   * @param {ArrayBuffer} buffer - Serialized database from exportToBuffer
   */
  async importFromBuffer(buffer) {
    await this.initialize();

    const view = new DataView(buffer);
    const bytes = new Uint8Array(buffer);
    let pos = 0;

    // Read header size (not used directly, just for validation)
    const headerSize = view.getUint32(pos, true);
    pos += 4;

    // Read area count
    const areaCount = view.getUint32(pos, true);
    pos += 4;

    // Clear existing data
    await this.clear();

    // Import each entry
    for (let i = 0; i < areaCount; i++) {
      // Read offset
      const offsetLow = view.getUint32(pos, true);
      const offsetHigh = view.getUint32(pos + 4, true);
      const offset = offsetLow + (offsetHigh * 0x100000000);
      pos += 8;

      // Read size
      const size = view.getUint32(pos, true);
      pos += 4;

      // Read data
      const data = buffer.slice(pos, pos + size);
      pos += size;

      // Store in IndexedDB
      await this.store(offset, data);
    }

    return { headerSize, areaCount };
  }

  /**
   * Clear all data in the database
   */
  async clear() {
    await this.initialize();

    return new Promise((resolve, reject) => {
      const tx = this.db.transaction(LEAVES_STORE_NAME, 'readwrite');
      const store = tx.objectStore(LEAVES_STORE_NAME);

      const request = store.clear();

      request.onsuccess = () => resolve();
      request.onerror = () => reject(new Error(`Clear failed: ${request.error}`));
    });
  }

  /**
   * Delete the entire database
   */
  static async deleteDatabase(dbName) {
    return new Promise((resolve, reject) => {
      const request = indexedDB.deleteDatabase(dbName);

      request.onsuccess = () => resolve();
      request.onerror = () => reject(new Error(`Delete database failed: ${request.error}`));
    });
  }

  /**
   * Get storage statistics
   * @returns {Promise<{keys: number, totalSize: number}>}
   */
  async getStats() {
    await this.initialize();

    const keys = await this.getAllKeys();
    let totalSize = 0;

    for (const key of keys) {
      const offset = key === 'header' ? 0 : parseInt(key.split('_')[1]);
      const data = await this.load(offset);
      if (data) {
        totalSize += data.byteLength;
      }
    }

    return { keys: keys.length, totalSize };
  }
}

/**
 * Register Emscripten bindings for C++ interop
 * Call this after the Emscripten module is loaded
 */
export function registerEmscriptenBindings(Module) {
  // Storage instance cache
  const storageInstances = new Map();

  // Get or create storage instance
  function getStorage(dbName) {
    if (!storageInstances.has(dbName)) {
      storageInstances.set(dbName, new LeavesStorage(dbName));
    }
    return storageInstances.get(dbName);
  }

  // Expose to Emscripten's FS or as global functions
  Module.leaves_idb_store = async function(dbName, key, dataPtr, size) {
    const storage = getStorage(dbName);
    const data = Module.HEAPU8.slice(dataPtr, dataPtr + size);
    await storage.store(parseInt(key.replace('area_', '')) || 0, data);
  };

  Module.leaves_idb_load = async function(dbName, key, dataPtr, size) {
    const storage = getStorage(dbName);
    const offset = key === 'header' ? 0 : parseInt(key.replace('area_', ''));
    const data = await storage.load(offset);

    if (data) {
      const src = new Uint8Array(data);
      const copySize = Math.min(size, src.length);
      Module.HEAPU8.set(src.subarray(0, copySize), dataPtr);
      return copySize;
    }
    return 0;
  };

  Module.leaves_idb_delete = async function(dbName, key) {
    const storage = getStorage(dbName);
    const offset = key === 'header' ? 0 : parseInt(key.replace('area_', ''));
    await storage.delete(offset);
  };

  Module.leaves_idb_close = function(dbName) {
    const storage = storageInstances.get(dbName);
    if (storage) {
      storage.close();
      storageInstances.delete(dbName);
    }
  };
}

// Export for different module systems
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { LeavesStorage, registerEmscriptenBindings };
}
