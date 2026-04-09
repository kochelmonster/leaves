/**
 * WebSocket replication client (WASM / Emscripten)
 *
 * Connects to a native server via WebSocket, receives replication data
 * into a BrowserStorage, then verifies the replicated keys.
 *
 * Build: emcmake cmake ... && cmake --build build-wasm -j
 * Run:   node --experimental-wasm-jspi tests/ws_replication/run.mjs
 *
 * Architecture — message queuing:
 *   JS onmessage pushes ArrayBuffers into Module._wsQueue[].
 *   C++ main loop calls emscripten_sleep(10), then drains the queue
 *   via leaves_ws_queue_size() / leaves_ws_dequeue() EM_JS helpers.
 *   This avoids re-entering WASM from async JS callbacks.
 */

#ifdef __EMSCRIPTEN__

#ifndef TESTING
#define TESTING
#endif

#include <emscripten.h>
#include <emscripten/val.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "leaves/replication.hpp"
#include "leaves/browserstore.hpp"
#include "leaves/intern/replication/_replication_db.hpp"

using namespace leaves;
using emscripten::val;

// ── JS-side message queue ───────────────────────────────────────
//
// WebSocket onmessage pushes ArrayBuffers into Module._wsQueue.
// C++ pulls them out synchronously between emscripten_sleep() calls.

EM_JS(void, leaves_ws_connect, (const char* url_ptr), {
  const url = UTF8ToString(url_ptr);
  Module._wsQueue = [];
  Module._wsOpen  = false;
  Module._wsDone  = false;

  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  Module._ws = ws;

  ws.onopen = () => {
    Module._wsOpen = true;
  };
  ws.onmessage = (ev) => {
    Module._wsQueue.push(new Uint8Array(ev.data));
  };
  ws.onclose = () => {
    Module._wsDone = true;
  };
  ws.onerror = (e) => {
    console.error("[client] ws error", e);
    Module._wsDone = true;
  };
});

EM_JS(int, leaves_ws_is_open, (), {
  return Module._wsOpen ? 1 : 0;
});

EM_JS(int, leaves_ws_is_done, (), {
  return Module._wsDone ? 1 : 0;
});

EM_JS(int, leaves_ws_queue_size, (), {
  return Module._wsQueue.length;
});

// Dequeue one message into a WASM buffer.  Returns actual byte count,
// or 0 if the queue is empty, or -1 if the message exceeds buf_size.
EM_JS(int, leaves_ws_dequeue, (void* buf, int buf_size), {
  if (!Module._wsQueue.length) return 0;
  const msg = Module._wsQueue.shift();
  if (msg.byteLength > buf_size) return -1;
  HEAPU8.set(msg, buf);
  return msg.byteLength;
});

EM_JS(void, leaves_ws_send, (const void* data, int size), {
  Module._ws.send(HEAPU8.slice(data, data + size));
});

EM_JS(void, leaves_force_exit, (int code), {
  if (typeof process !== "undefined") process.exit(code);
});

// ── Transport adapter ───────────────────────────────────────────

struct WasmWsTransport : ReplicationTransport {
  void send(const uint8_t* data, size_t size) override {
    leaves_ws_send(data, static_cast<int>(size));
  }
};

// ── Test helpers ────────────────────────────────────────────────

static int g_failures = 0;

#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__);    \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b, msg)                                              \
  do {                                                                   \
    if ((a) != (b)) {                                                    \
      std::fprintf(stderr, "  FAIL: %s — got '%s', expected '%s'\n",     \
                   msg, std::string(a).c_str(), std::string(b).c_str()); \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// ── Main ────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: client <port>\n");
    return 1;
  }

  std::string url = std::string("ws://localhost:") + argv[1];
  std::fprintf(stderr, "[client] connecting to %s\n", url.c_str());

  leaves_ws_connect(url.c_str());

  // Wait for WebSocket to open
  while (!leaves_ws_is_open() && !leaves_ws_is_done()) {
    emscripten_sleep(10);
  }
  if (leaves_ws_is_done()) {
    std::fprintf(stderr, "[client] ws failed to connect\n");
    return 1;
  }
  std::fprintf(stderr, "[client] connected\n");

  // Create receiver-side storage
  auto dst_storage = BrowserStorage::create("test_ws_repl");
  auto dst_db = dst_storage->open<_ReplicationDB>("testdb");

  WasmWsTransport transport;

  struct Events : ReplicationEvents {
    bool completed = false;
    bool errored   = false;
    void on_complete(uint64_t, size_t n) override {
      std::fprintf(stderr, "[client] complete, %zu nodes\n", n);
      completed = true;
    }
    void on_error(uint64_t, ReplicationError, const char* r) override {
      std::fprintf(stderr, "[client] error: %s\n", r);
      errored = true;
    }
    void on_progress(uint64_t, size_t, size_t) override {}
  } events;

  ReplicationReceiver<BrowserStorage> receiver(dst_db);
  receiver.begin(&transport, &events);

  // Receive buffer for pulling messages from JS queue
  constexpr size_t MAX_MSG = 4 * 1024 * 1024;  // 4 MB
  std::vector<uint8_t> msg_buf(MAX_MSG);

  // Protocol loop — drain JS queue, feed receiver FSM
  while (receiver.state() == ReplicationState::ACTIVE) {
    // Pull all queued messages
    while (leaves_ws_queue_size() > 0) {
      int n = leaves_ws_dequeue(msg_buf.data(), static_cast<int>(msg_buf.size()));
      if (n <= 0) {
        std::fprintf(stderr, "[client] dequeue error %d\n", n);
        break;
      }

      // Feed into receiver's zero-copy buffer
      auto& rb = receiver.receive_buffer();
      size_t todo = static_cast<size_t>(n);
      size_t off  = 0;
      while (off < todo) {
        size_t chunk = std::min(todo - off, rb.available());
        if (chunk == 0) break;  // shouldn't happen
        std::memcpy(rb.write_ptr(), msg_buf.data() + off, chunk);
        rb.advance(chunk);
        off += chunk;
        receiver.on_data_received();
      }

      // Check if we're done after processing
      if (receiver.state() != ReplicationState::ACTIVE) break;
    }

    if (receiver.state() == ReplicationState::ACTIVE) {
      emscripten_sleep(10);
    }
  }

  std::fprintf(stderr, "[client] replication finished\n");

  // ── Verify replicated data ────────────────────────────────────
  CHECK(events.completed, "replication completed");
  CHECK(!events.errored,  "no errors");

  {
    auto c = dst_db.cursor();
    c.find(Slice("hello"));
    CHECK(c.is_valid(),                     "key 'hello' exists");
    CHECK_EQ(c.value().string(), "world",   "hello → world");

    c.find(Slice("foo"));
    CHECK(c.is_valid(),                     "key 'foo' exists");
    CHECK_EQ(c.value().string(), "bar",     "foo → bar");

    c.find(Slice("count"));
    CHECK(c.is_valid(),                     "key 'count' exists");
    CHECK_EQ(c.value().string(), "12345",   "count → 12345");
  }

  if (g_failures == 0) {
    std::printf("PASS — 3 keys replicated via WebSocket\n");
  } else {
    std::printf("FAIL — %d check(s) failed\n", g_failures);
  }

  int rc = g_failures == 0 ? 0 : 1;
  leaves_force_exit(rc);
  return rc;
}

#else   // !__EMSCRIPTEN__
#include <cstdio>
int main() {
  std::fprintf(stderr, "This test requires Emscripten.\n");
  return 77;  // skip
}
#endif  // __EMSCRIPTEN__
