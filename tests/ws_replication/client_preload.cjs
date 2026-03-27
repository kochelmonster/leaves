// Preload script for WASM client in Node.js
// Injects WebSocket and IndexedDB globals needed by Emscripten
"use strict";
const { WebSocket } = require("ws");
require("fake-indexeddb/auto");
globalThis.WebSocket = WebSocket;
