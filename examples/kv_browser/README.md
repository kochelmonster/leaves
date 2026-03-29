# KV Browser Demo

Interactive key-value store that runs in the browser with bidirectional
replication to a native server over WebSocket.

Open multiple browser tabs to see changes sync in real time.

## Architecture

```
┌─────────────────────┐       WebSocket        ┌──────────────────────┐
│   Browser (WASM)    │◄──────────────────────►│   Native Server      │
│                     │    LVRP replication    │                      │
│  ReplicatingBrowser │                        │  ReplicatingMap      │
│  Storage (IndexedDB)│                        │  Storage (mmap)      │
└─────────────────────┘                        └──────────────────────┘
```

- **Server** (`server.cpp`): Native C++ process using Boost.Beast for
  WebSocket. Hosts a `ReplicatingMapStorage` and handles multiple clients
  concurrently (thread-per-client, mutex-serialized replication). A
  server-specific aspect debounces post-commit `SYNC` hints so bursts of
  commits collapse into a single client refresh notification.

- **Client** (`client.cpp`): Emscripten/WASM module using Embind to expose
  a `KVDemo` API to JavaScript. Local data lives in IndexedDB via
  `ReplicatingBrowserStorage`.

- **UI** (`index.html`): Single-page app with a dark-themed table view.
  Add, remove, and browse key-value pairs. Tabs react to server-pushed
  `SYNC` hints and then run a sync cycle.

## Prerequisites

- CMake, Ninja, GCC/Clang (for the native server)
- Emscripten SDK (for the WASM client)
- Node.js 18+ (for the runner script)
- Chrome 128+ or another browser with [JSPI](https://v8.dev/blog/jspi) support

## Build

```bash
# Native server
cmake --build build -j --target kv_demo_server

# WASM client (configure once, then build)
emcmake cmake -B build-wasm -G Ninja
cmake --build build-wasm -j --target kv_demo_client
```

## Run

```bash
node examples/kv_browser/run.mjs
```

This starts the native WebSocket server, an HTTP server with the required
COOP/COEP headers, and opens your browser.

Options:

| Flag            | Default | Description          |
|-----------------|---------|----------------------|
| `--ws-port`     | 19876   | WebSocket server port|
| `--http-port`   | 8080    | HTTP server port     |
| `--server`      | `build/kv_demo_server` | Path to server binary |
| `--wasm-dir`    | `build-wasm` | Directory with WASM artifacts |

## Sync Protocol

Each sync cycle follows this sequence:

1. Client sends text `"SYNC"`
2. Server → Client: `ReplicationSender` streams binary LVRP data
3. Server sends text `"PULL"`
4. Client → Server: `ReplicationSender` streams binary LVRP data
5. Server sends text `"DONE"`

This is a full bidirectional exchange — both sides send and receive changes
in a single cycle.

After a successful server-side commit, the native server defers a text
`"SYNC"` notification briefly before broadcasting it to the other connected
clients. The delay coalesces rapid commit bursts into a single refresh hint.
