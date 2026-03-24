// Embind bindings for leaves _BrowserStore
//
// Exposes LeavesStore, LeavesDB, and LeavesCursor to JavaScript.
// Build: emcmake cmake -B build-wasm && cmake --build build-wasm -j
// Usage: see js/leaves.mjs or docs/BROWSER_STORAGE.md

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <leaves/intern/storage/_browserstore.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace emscripten;

// ── Type aliases ─────────────────────────────────────────────────

using Store = leaves::_BrowserStore;
using DB = Store::DB;
using Cursor = DB::Cursor;
using cursor_ptr = DB::cursor_ptr;

// Factory — embind passes std::string; constructor wants const char*
static Store* make_store(const std::string& name, uint16_t db_count, size_t capacity) {
  return new Store(name.c_str(), db_count, capacity, 0);
}

// Close — async IDB flush. Must be called before .delete() to avoid crash.
static void store_close(Store& s) {
  s.destroy();
}

// Number of IDB write operations still in flight (global counter)
static int store_pending_writes() {
  return leaves_pending_writes();
}

// ── Wrapper functions ────────────────────────────────────────────
// Slice is a non-owning view — all JS conversions must copy data.

static DB* store_db(Store& s, const std::string& name) {
  return s[name.c_str()];
}

static std::vector<std::string> store_list_dbs(Store& s) {
  std::vector<std::string> result;
  s.list_dbs(result);
  return result;
}

static val store_export(Store& s) {
  auto buf = s.export_to_buffer();
  return val(typed_memory_view(buf.size(), reinterpret_cast<const uint8_t*>(buf.data()))).call<val>("slice");
}

static void store_import(Store& s, const std::string& data) {
  std::vector<char> buf(data.begin(), data.end());
  s.import_from_buffer(buf);
}

// Cursor key/value — string variants
static std::string cursor_key(Cursor& c) {
  leaves::Slice k = c.key();
  return std::string(k.data(), k.size());
}

static std::string cursor_get_value(Cursor& c) {
  leaves::Slice v = c.value();
  return std::string(v.data(), v.size());
}

static void cursor_set_value(Cursor& c, const std::string& v) {
  c.value(leaves::Slice(v.data(), v.size()));
}

// Cursor key/value — binary (Uint8Array) variants
static val cursor_key_bytes(Cursor& c) {
  leaves::Slice k = c.key();
  return val(typed_memory_view(k.size(), reinterpret_cast<const uint8_t*>(k.data()))).call<val>("slice");
}

static val cursor_get_value_bytes(Cursor& c) {
  leaves::Slice v = c.value();
  return val(typed_memory_view(v.size(), reinterpret_cast<const uint8_t*>(v.data()))).call<val>("slice");
}

static void cursor_set_value_bytes(Cursor& c, const std::string& v) {
  // Embind converts Uint8Array to std::string automatically
  c.value(leaves::Slice(v.data(), v.size()));
}

// Cursor find — the key must persist because cursor.rest_key (a Slice) keeps
// a non-owning reference used by subsequent reserve()/insert operations.
// WASM is single-threaded so a static buffer is safe.
static std::string g_find_buffer;
static void cursor_find(Cursor& c, const std::string& key) {
  g_find_buffer = key;
  c.find(leaves::Slice(g_find_buffer.data(), g_find_buffer.size()));
}

// Cursor navigation — wrap base-class methods (CRTP prevents direct member ptrs)
static bool cursor_is_valid(const Cursor& c) { return c.is_valid(); }
static void cursor_first(Cursor& c) { c.first(); }
static void cursor_last(Cursor& c) { c.last(); }
static void cursor_next(Cursor& c) { c.next(); }
static void cursor_prev(Cursor& c) { c.prev(); }
static void cursor_remove(Cursor& c) { c.remove(); }
static bool cursor_commit(Cursor& c, bool sync) { return c.commit(sync); }
static bool cursor_rollback(Cursor& c) { return c.rollback(); }

// ── Embind registrations ─────────────────────────────────────────
// Functions that may trigger IDB (cache miss / write / init) are marked async()
// so JSPI wraps them with WebAssembly.promising — they return Promises in JS.

EMSCRIPTEN_BINDINGS(leaves) {

  register_vector<std::string>("VectorString");

  class_<Store>("LeavesStore")
      .class_function("create", &make_store, allow_raw_pointers(), async())
      .class_function("pendingWrites", &store_pending_writes)
      .function("close", &store_close, async())
      .function("db", &store_db, allow_raw_pointers(), async())
      .function("listDbs", &store_list_dbs)
      .function("exportToBuffer", &store_export)
      .function("importFromBuffer", &store_import, async())
      .function("clearDatabase", &Store::clear_database, async());

  class_<DB>("LeavesDB")
      .function("createCursor", &DB::create_cursor, async());

  class_<Cursor>("LeavesCursor")
      .smart_ptr<cursor_ptr>("LeavesCursorPtr")
      .function("find", &cursor_find, async())
      .function("first", &cursor_first, async())
      .function("last", &cursor_last, async())
      .function("next", &cursor_next, async())
      .function("prev", &cursor_prev, async())
      .function("isValid", &cursor_is_valid)
      .function("key", &cursor_key)
      .function("getValue", &cursor_get_value, async())
      .function("setValue", &cursor_set_value, async())
      .function("keyBytes", &cursor_key_bytes)
      .function("getValueBytes", &cursor_get_value_bytes, async())
      .function("setValueBytes", &cursor_set_value_bytes, async())
      .function("remove", &cursor_remove, async())
      .function("commit", &cursor_commit, async())
      .function("rollback", &cursor_rollback, async());
}

#endif  // __EMSCRIPTEN__
