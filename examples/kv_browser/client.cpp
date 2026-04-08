/**
 * KV Browser Demo — WASM client with Embind
 *
 * Exposes a KVDemo class to JavaScript that wraps:
 *   - BrowserStorage (IndexedDB) for local key-value storage
 *   - Bidirectional WebSocket replication with a native server
 *
 * The sync protocol per cycle:
 *   1. Client sends text "SYNC"
 *   2. Server→Client: server runs ReplicationSender (binary LVRP)
 *   3. Server sends text "PULL"
 *   4. Client→Server: client runs ReplicationSender (binary LVRP)
 *   5. Server sends text "DONE"
 *
 * Build: emcmake cmake -B build-wasm && cmake --build build-wasm --target kv_demo_client
 */

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "leaves/replication.hpp"
#include "leaves/browserstore.hpp"
#include "leaves/intern/replication/_replication_db.hpp"

using namespace leaves;
using namespace emscripten;

// ── JS-side WebSocket bridge ────────────────────────────────────
//
// We maintain TWO queues: binary messages (LVRP data) and text
// messages (control: "PULL", "DONE", "SYNC" notifications).

EM_JS(void, ws_connect, (const char* url_ptr), {
  const url = UTF8ToString(url_ptr);
  Module._wsBinQueue  = [];
  Module._wsTextQueue = [];
  Module._wsOpen = false;
  Module._wsDone = false;

  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  Module._ws = ws;

  ws.onopen = () => { Module._wsOpen = true; };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      Module._wsTextQueue.push(ev.data);
    } else {
      Module._wsBinQueue.push(new Uint8Array(ev.data));
    }
  };
  ws.onclose = () => { Module._wsDone = true; };
  ws.onerror = (e) => {
    console.error("[kv_demo] ws error", e);
    Module._wsDone = true;
  };
});

EM_JS(int, ws_is_open, (), { return Module._wsOpen ? 1 : 0; });
EM_JS(int, ws_is_done, (), { return Module._wsDone ? 1 : 0; });

// Binary message queue (LVRP data)
EM_JS(int, ws_bin_queue_size, (), { return Module._wsBinQueue.length; });

EM_JS(int, ws_bin_dequeue, (void* buf, int buf_size), {
  if (!Module._wsBinQueue.length) return 0;
  const msg = Module._wsBinQueue.shift();
  if (msg.byteLength > buf_size) return -1;
  HEAPU8.set(msg, buf);
  return msg.byteLength;
});

EM_JS(void, ws_send_binary, (const void* data, int size), {
  Module._ws.send(HEAPU8.slice(data, data + size));
});

// Text message queue (control messages)
EM_JS(int, ws_text_queue_size, (), { return Module._wsTextQueue.length; });

EM_JS(void, ws_text_dequeue, (char* buf, int buf_size), {
  if (!Module._wsTextQueue.length) {
    HEAPU8[buf] = 0;
    return;
  }
  const msg = Module._wsTextQueue.shift();
  stringToUTF8(msg, buf, buf_size);
});

EM_JS(void, ws_send_text, (const char* ptr), {
  Module._ws.send(UTF8ToString(ptr));
});

// ── Transport adapter ───────────────────────────────────────────

struct WasmWsTransport : ReplicationTransport {
  void send(const uint8_t* data, size_t size) override {
    ws_send_binary(data, static_cast<int>(size));
  }
};

// ── Replication helpers ─────────────────────────────────────────

static constexpr size_t MAX_MSG = 4 * 1024 * 1024;

// Run receiver FSM loop: drain binary WS queue until FSM leaves ACTIVE
static bool run_receiver_loop(
    ReplicationReceiver<BrowserStorage>& receiver,
    std::vector<uint8_t>& msg_buf) {
  while (receiver.state() == ReplicationState::ACTIVE) {
    while (ws_bin_queue_size() > 0) {
      int n = ws_bin_dequeue(msg_buf.data(), static_cast<int>(msg_buf.size()));
      if (n <= 0) break;

      auto& rb = receiver.receive_buffer();
      size_t todo = static_cast<size_t>(n);
      size_t off = 0;
      while (off < todo) {
        size_t chunk = std::min(todo - off, rb.available());
        if (chunk == 0) break;
        std::memcpy(rb.write_ptr(), msg_buf.data() + off, chunk);
        rb.advance(chunk);
        off += chunk;
        receiver.on_data_received();
      }

      if (receiver.state() != ReplicationState::ACTIVE) break;
    }
    if (receiver.state() == ReplicationState::ACTIVE) {
      emscripten_sleep(10);
    }
    if (ws_is_done()) return false;
  }
  return receiver.state() == ReplicationState::IDLE;
}

// Run sender FSM loop: the sender pushes messages via transport.send(),
// and we read binary responses from the WS queue
static bool run_sender_loop(
    ReplicationSender<BrowserStorage>& sender,
    std::vector<uint8_t>& msg_buf) {
  while (sender.state() == ReplicationState::ACTIVE) {
    while (ws_bin_queue_size() > 0) {
      int n = ws_bin_dequeue(msg_buf.data(), static_cast<int>(msg_buf.size()));
      if (n <= 0) break;
      sender.on_message_received(msg_buf.data(), static_cast<size_t>(n));
      if (sender.state() != ReplicationState::ACTIVE) break;
    }
    if (sender.state() == ReplicationState::ACTIVE) {
      emscripten_sleep(10);
    }
    if (ws_is_done()) return false;
  }
  return sender.state() == ReplicationState::IDLE;
}

// Wait for a specific text control message, yielding via emscripten_sleep
static bool wait_for_text(const char* expected) {
  char buf[64];
  while (true) {
    if (ws_text_queue_size() > 0) {
      ws_text_dequeue(buf, sizeof(buf));
      if (std::strcmp(buf, expected) == 0) return true;
      // Unexpected control message — could be a stale SYNC notification, skip
      continue;
    }
    if (ws_is_done()) return false;
    emscripten_sleep(10);
  }
}

// ── KVDemo class ────────────────────────────────────────────────

struct KVDemo {
  BrowserStorage::storage_ptr _storage;
  TDB<BrowserStorage, _ReplicationDB> _db;
  WasmWsTransport _transport;
  std::vector<uint8_t> _msg_buf;
  bool _connected = false;
  bool _has_notification = false;

  KVDemo(const std::string& name)
      : _storage(BrowserStorage::create(name.c_str(), 10*1024*1024)),
        _db(_storage->open<_ReplicationDB>("main")),
        _msg_buf(MAX_MSG) {}

  // Connect to WebSocket server
  void connect(const std::string& url) {
    ws_connect(url.c_str());
    while (!ws_is_open() && !ws_is_done()) {
      emscripten_sleep(10);
    }
    _connected = ws_is_open();

    if (_connected) {
      // Start background notification polling via JS
      // (checks for server-pushed "SYNC" text messages)
      _drain_text_notifications();
    }
  }

  bool is_connected() const { return _connected && !ws_is_done(); }

  // Put a key-value pair into local DB
  void put(const std::string& key, const std::string& value) {
    auto c = _db.cursor();
    c.find(Slice(key.data(), key.size()));
    c.value(Slice(value.data(), value.size()));
    c.commit();
  }

  // Remove a key from local DB
  void remove(const std::string& key) {
    auto c = _db.cursor();
    c.find(Slice(key.data(), key.size()));
    if (c.is_valid()) {
      c.remove();
      c.commit();
    }
  }

  // Get all key-value pairs as a JS array of {key, value} objects
  val getAll() {
    val arr = val::array();
    auto c = _db.cursor();
    c.first();
    int i = 0;
    while (c.is_valid()) {
      val obj = val::object();
      Slice k = c.key();
      Slice v = c.value();
      obj.set("key", std::string(k.data(), k.size()));
      obj.set("value", std::string(v.data(), v.size()));
      arr.set(i++, obj);
      c.next();
    }
    return arr;
  }

  // Check for and consume a notification from the server
  bool hasNotification() {
    _drain_text_notifications();
    bool n = _has_notification;
    _has_notification = false;
    return n;
  }

  // Run a full bidirectional sync cycle
  bool sync() {
    if (!is_connected()) return false;

    // Cancel background purge to prevent JSPI reentrancy.
    // JSPI suspensions (WS reads/writes, IDB loads) yield to the browser
    // event loop, which can fire the purge setTimeout callback. Since the
    // browser Mutex is a no-op, the purge would reentrantly modify the
    // database while sync is suspended, corrupting the trie.
    _db._internal()->cancel_purge();

    // Drain any stale text notifications before sync
    _drain_text_notifications();

    // 1. Send "SYNC" to initiate
    ws_send_text("SYNC");

    // 2. Phase 1: Receive from server (server sends, we receive)
    struct Events : ReplicationEvents {
      void on_complete(uint64_t, size_t) override {}
      void on_error(uint64_t, ReplicationError, const char* r) override {
        std::fprintf(stderr, "[kv_demo] repl error: %s\n", r);
      }
      void on_progress(uint64_t, size_t, size_t) override {}
    } events;

    {
      ReplicationReceiver<BrowserStorage> receiver(_db);
      receiver.begin(&_transport, &events);
      if (!run_receiver_loop(receiver, _msg_buf)) {
        std::fprintf(stderr, "[kv_demo] receiver failed\n");
        _db._internal()->start_purge();
        return false;
      }
    }

    // 3. Wait for "PULL" from server
    if (!wait_for_text("PULL")) {
      std::fprintf(stderr, "[kv_demo] did not receive PULL\n");
      _db._internal()->start_purge();
      return false;
    }

    // 4. Phase 2: Send to server (we send, server receives)
    {
      ReplicationSender<BrowserStorage> sender(_db);
      sender.begin(&_transport, &events);
      if (!run_sender_loop(sender, _msg_buf)) {
        std::fprintf(stderr, "[kv_demo] sender failed\n");
        _db._internal()->start_purge();
        return false;
      }
    }

    // 5. Wait for "DONE" from server
    if (!wait_for_text("DONE")) {
      std::fprintf(stderr, "[kv_demo] did not receive DONE\n");
      _db._internal()->start_purge();
      return false;
    }

    _db._internal()->start_purge();
    return true;
  }

 private:
  void _drain_text_notifications() {
    char buf[64];
    while (ws_text_queue_size() > 0) {
      ws_text_dequeue(buf, sizeof(buf));
      if (std::strcmp(buf, "SYNC") == 0) {
        _has_notification = true;
      }
    }
  }
};

// ── Static instance ─────────────────────────────────────────────
// Single-threaded WASM — one global instance is safe.

static std::unique_ptr<KVDemo> g_demo;
static std::string g_find_buffer;

static void demo_create(const std::string& name) {
  g_demo = std::make_unique<KVDemo>(name);
}

static void demo_connect(const std::string& url) {
  g_demo->connect(url);
}

static bool demo_is_connected() {
  return g_demo && g_demo->is_connected();
}

static void demo_put(const std::string& key, const std::string& value) {
  g_demo->put(key, value);
}

static void demo_remove(const std::string& key) {
  g_demo->remove(key);
}

static val demo_get_all() {
  return g_demo->getAll();
}

static bool demo_sync() {
  return g_demo->sync();
}

static bool demo_has_notification() {
  return g_demo->hasNotification();
}

// ── Embind registrations ────────────────────────────────────────

EMSCRIPTEN_BINDINGS(kv_demo) {
  function("kvCreate", &demo_create, async());
  function("kvConnect", &demo_connect, async());
  function("kvIsConnected", &demo_is_connected);
  function("kvPut", &demo_put, async());
  function("kvRemove", &demo_remove, async());
  function("kvGetAll", &demo_get_all, async());
  function("kvSync", &demo_sync, async());
  function("kvHasNotification", &demo_has_notification);
}

#endif  // __EMSCRIPTEN__
