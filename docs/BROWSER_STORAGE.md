# Leaves Browser Storage

WebAssembly-compatible storage layer using IndexedDB for browser environments.

## Architecture

`_BrowserStore` provides the same interface as `_FileStore` but uses IndexedDB instead of file I/O:

```
┌─────────────────────────────────────────────────────────────┐
│                     _BrowserStore                           │
│  (inherits from _CacheStore<_BrowserStoreTraits, _BrowserOps>)│
├─────────────────────────────────────────────────────────────┤
│                    _BrowserOperations                       │
│  - open(db_name)     → opens IndexedDB database             │
│  - read(offset,...)  → emscripten_idb_load()               │
│  - write(offset,...) → emscripten_idb_store()              │
│  - close()           → flush and close                      │
├─────────────────────────────────────────────────────────────┤
│                      IndexedDB                              │
│  Object Store: "leaves_data"                                │
│  Keys: "header", "area_<offset>"                            │
│  Values: ArrayBuffer (raw bytes)                            │
└─────────────────────────────────────────────────────────────┘
```

## Storage Model

IndexedDB stores key-value pairs where:
- **Key**: String identifier (`"header"` or `"area_<offset>"`)
- **Value**: Raw bytes as ArrayBuffer

This maps to the file-based model:
| File Storage | Browser Storage |
|--------------|-----------------|
| File offset 0 | Key `"header"` |
| File offset N | Key `"area_N"` |
| truncate() | No-op (auto-grows) |
| fstream | IndexedDB transactions |

## Build Requirements

### Emscripten Setup

```bash
# Install Emscripten SDK
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### CMake Configuration

```cmake
# Add to CMakeLists.txt for WebAssembly build
if(EMSCRIPTEN)
  set(CMAKE_EXECUTABLE_SUFFIX ".js")
  
  # Enable Asyncify for synchronous IndexedDB access
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
    -sASYNCIFY \
    -sASYNCIFY_IMPORTS=['emscripten_idb_load','emscripten_idb_store','emscripten_idb_delete'] \
    -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap'] \
    -sEXPORT_ES6=1 \
    -sMODULARIZE=1 \
    -sENVIRONMENT=web")
endif()
```

### Build Commands

```bash
# Configure for WebAssembly
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build-wasm
```

## Usage

### C++ Side

```cpp
#include <leaves/intern/storage/_browserstore.hpp>

// Create browser-backed storage
leaves::_BrowserStore store("my_database", 16, 100 * leaves::M);

// Get or create a database
auto* db = store["users"];

// Use like normal
auto txn = db->begin();
txn->put("key1", "value1");
txn->commit();
```

### JavaScript Side

```javascript
import { LeavesStorage } from './leaves_browser_storage.js';

// Direct JavaScript usage (without WASM)
const storage = new LeavesStorage('my_database');
await storage.initialize();

// Export database to file
const data = await storage.exportToBuffer();
const blob = new Blob([data], { type: 'application/octet-stream' });
const url = URL.createObjectURL(blob);
// Download blob...

// Import database from file
const buffer = await file.arrayBuffer();
await storage.importFromBuffer(buffer);

// Get statistics
const stats = await storage.getStats();
console.log(`Keys: ${stats.keys}, Size: ${stats.totalSize} bytes`);
```

### HTML Integration

```html
<!DOCTYPE html>
<html>
<head>
  <title>Leaves Browser Demo</title>
</head>
<body>
  <script type="module">
    import { LeavesStorage, registerEmscriptenBindings } from './leaves_browser_storage.js';
    import createModule from './leaves_wasm.js';

    async function init() {
      // Load WASM module
      const Module = await createModule();
      
      // Register IndexedDB bindings
      registerEmscriptenBindings(Module);
      
      // Now use the C++ API through Module
      // Module._create_store("my_db");
      // ...
    }

    init().catch(console.error);
  </script>
</body>
</html>
```

## API Reference

### _BrowserStore

```cpp
// Constructor
_BrowserStore(
  const char* db_name,      // IndexedDB database name
  uint16_t db_count = 48,   // Max number of sub-databases
  size_t capacity = 100*M,  // Cache capacity in bytes
  size_t pool_threads = 0   // Thread pool size (0 for browser)
);

// Database access
DB* operator[](const char* name);  // Get/create database
DB* make(const char* name);        // Same as operator[]
void remove_db(const char* name);  // Delete database

// Lifecycle
void flush(bool sync = false);     // Write dirty blocks
void close();                      // Close connection

// Browser-specific
std::vector<char> export_to_buffer() const;  // Export all data
void import_from_buffer(const std::vector<char>& data);
void clear_database();             // Delete all data
```

### LeavesStorage (JavaScript)

```typescript
class LeavesStorage {
  constructor(dbName: string);
  
  // Core operations
  async initialize(): Promise<void>;
  async store(offset: number, data: ArrayBuffer): Promise<void>;
  async load(offset: number): Promise<ArrayBuffer | null>;
  async delete(offset: number): Promise<void>;
  close(): void;
  
  // Import/Export
  async exportToBuffer(): Promise<ArrayBuffer>;
  async importFromBuffer(buffer: ArrayBuffer): Promise<void>;
  
  // Utilities
  async getAllKeys(): Promise<string[]>;
  async getStats(): Promise<{keys: number, totalSize: number}>;
  async clear(): Promise<void>;
  static async deleteDatabase(dbName: string): Promise<void>;
}
```

## Limitations

1. **Single-threaded**: Browser JavaScript is single-threaded; `pool_threads` should be 0
2. **Async operations**: Uses Emscripten Asyncify which has stack size limitations
3. **Storage quotas**: IndexedDB has browser-specific storage limits (typically 50% of disk)
4. **No file system**: Cannot use file paths; databases are named strings

## Memory Tuning

For browser environments, use smaller area sizes:

```cpp
// _BrowserStoreTraits defaults
static constexpr size_t AREA_SIZE = 64 * K;    // vs 128K for file
static constexpr size_t MAX_KEY_SIZE = 512 * K; // vs 1M for file
```

Adjust cache capacity based on available memory:

```cpp
// Modest browser (mobile)
_BrowserStore store("db", 16, 50 * M);

// Desktop browser
_BrowserStore store("db", 32, 200 * M);
```

## Debugging

Enable IndexedDB debugging in browser DevTools:
- Chrome: Application → IndexedDB
- Firefox: Storage → IndexedDB

Check storage usage:
```javascript
const estimate = await navigator.storage.estimate();
console.log(`Used: ${estimate.usage}, Quota: ${estimate.quota}`);
```

## Testing

```bash
# Run tests in browser via emrun
emrun --browser chrome build-wasm/test_browser.html

# Or use Puppeteer/Playwright for headless testing
npx playwright test browser-storage.test.js
```
