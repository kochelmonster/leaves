// Embind bindings for leaves _BrowserStore
//
// Exposes LeavesStore, LeavesDB, LeavesCursor, and
// ReplicationSender/Receiver to JavaScript.
//
// The underlying storage always uses _BrowserStore<JSAspect> so that
// all databases (both regular and replication) support aspect callbacks.
// JSAspect is a no-op pass-through when no callbacks are set, so this
// is fully backward-compatible with existing non-aspect code.
//
// Build: emcmake cmake -B build-wasm && cmake --build build-wasm -j
// Usage: see js/leaves.mjs or docs/BROWSER_STORAGE.md
//
// JS API (matching C++ API):
//   var store = await Module.LeavesStore.create('storage_name', capacity);
//   var db    = await store.open('database_name');
//   var c     = db.createCursor();

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <leaves/intern/storage/_browserstore.hpp>
#include <memory>
#include <string>
#include <vector>

#include "_js_aspect.hpp"

using namespace emscripten;

#include "leaves/db.hpp"
#include "leaves/replication.hpp"

// ── Storage wrapper ────────────────────────────────────────────────
// Always uses _BrowserStore<JSAspect> so all DB types support aspects.

struct BrowserStoreWrapper {
  using StorageImpl = leaves::_BrowserStore<leaves::JSAspect>;
  using storage_ptr = std::shared_ptr<BrowserStoreWrapper>;

  std::shared_ptr<StorageImpl> _storage;

  BrowserStoreWrapper(const char* db_name, size_t capacity,
                      size_t pool_threads) {
    _storage = std::make_shared<StorageImpl>(db_name, capacity, pool_threads);
  }

  StorageImpl* operator->() { return _storage.get(); }
};

using Store = leaves::TDB<BrowserStoreWrapper>;
using DB = Store::db_type;
using Cursor = Store::Cursor;
using cursor_ptr = std::shared_ptr<Cursor>;
using ReplicationDB = leaves::TDB<BrowserStoreWrapper, leaves::_ReplicationDB>;
using ReplicationDBImpl = ReplicationDB::db_type;
using ReplicationCursorImpl = typename ReplicationDBImpl::Cursor;
using replication_cursor_ptr = std::shared_ptr<ReplicationCursorImpl>;

// ── Pure storage wrapper (no auto-opened DB) ──────────────────────
// Matches the C++ pattern: create a storage, then open databases within it.

struct JSStore {
  BrowserStoreWrapper::storage_ptr _storage;

  BrowserStoreWrapper::StorageImpl* operator->() {
    return _storage->_storage.get();
  }
};

// A database handle: storage + DB pointer (opened within that storage)
// All DBs opened through LeavesStore are aspect-capable.
struct JSDB {
  BrowserStoreWrapper::storage_ptr storage;
  DB* db;
};

// ── ReplicationDBCursor ──────────────────────────────────────────
// Wraps _ReplicationDB::Cursor (_ReplicationCursor) for use from JS.
// Mirrors the LeavesCursor API for compatibility.

struct ReplicationDBCursor {
  replication_cursor_ptr _cursor;
  BrowserStoreWrapper::storage_ptr _storage;

  ReplicationDBCursor(BrowserStoreWrapper::storage_ptr storage,
                      ReplicationDBImpl* db)
      : _storage(storage), _cursor(db->create_cursor()) {}

  // Buffer for find key persistence (see cursor_find)
  std::string _find_buffer;

  bool is_valid() const { return _cursor->is_valid(); }

  std::string key() const {
    leaves::Slice k = _cursor->key();
    return std::string(k.data(), k.size());
  }

  std::string get_value() const {
    leaves::Slice v = _cursor->value();
    return std::string(v.data(), v.size());
  }

  void set_value(const std::string& v) {
    _cursor->value(leaves::Slice(v.data(), v.size()));
  }

  val key_bytes() const {
    leaves::Slice k = _cursor->key();
    return val(typed_memory_view(k.size(),
                                 reinterpret_cast<const uint8_t*>(k.data())))
        .call<val>("slice");
  }

  val get_value_bytes() const {
    leaves::Slice v = _cursor->value();
    return val(typed_memory_view(v.size(),
                                 reinterpret_cast<const uint8_t*>(v.data())))
        .call<val>("slice");
  }

  void set_value_bytes(const std::string& v) {
    _cursor->value(leaves::Slice(v.data(), v.size()));
  }

  void find(const std::string& key) {
    _find_buffer = key;
    _cursor->find(leaves::Slice(_find_buffer.data(), _find_buffer.size()));
  }

  void first() { _cursor->first(); }
  void last() { _cursor->last(); }
  void next() { _cursor->next(); }
  void prev() { _cursor->prev(); }
  void remove() { _cursor->remove(); }
  bool commit(bool sync) { return _cursor->commit(sync); }
  bool rollback() { return _cursor->rollback(); }
  bool start_transaction(bool non_blocking) {
    return _cursor->start_transaction(non_blocking);
  }
  void update() { _cursor->update(); }

  // Return the per-cursor JS context object (aspect support)
  val aspect_context() const { return _cursor->_aspect_context.js_context; }

  bool is_transaction_active() const {
    return _cursor->is_transaction_active();
  }
};
// ── CursorWrapper ──────────────────────────────────────────────────
// Wraps the regular Cursor (TCursor<BrowserStoreWrapper>) for use from JS.
// Mirrors the ReplicationDBCursor API for compatibility (both use
// the modern member-function style).

struct CursorWrapper {
  Cursor _cursor;
  BrowserStoreWrapper::storage_ptr _storage;

  CursorWrapper(BrowserStoreWrapper::storage_ptr storage, DB* db)
      : _storage(storage), _cursor(storage, db) {}

  // Buffer for find key persistence (see cursor_find)
  std::string _find_buffer;

  bool is_valid() const { return _cursor.is_valid(); }

  std::string key() const {
    leaves::Slice k = _cursor.key();
    return std::string(k.data(), k.size());
  }

  std::string get_value() const {
    leaves::Slice v = _cursor.value();
    return std::string(v.data(), v.size());
  }

  void set_value(const std::string& v) {
    _cursor.value(leaves::Slice(v.data(), v.size()));
  }

  val key_bytes() const {
    leaves::Slice k = _cursor.key();
    return val(typed_memory_view(k.size(),
                                 reinterpret_cast<const uint8_t*>(k.data())))
        .call<val>("slice");
  }

  val get_value_bytes() const {
    leaves::Slice v = _cursor.value();
    return val(typed_memory_view(v.size(),
                                 reinterpret_cast<const uint8_t*>(v.data())))
        .call<val>("slice");
  }

  void set_value_bytes(const std::string& v) {
    _cursor.value(leaves::Slice(v.data(), v.size()));
  }

  void find(const std::string& key) {
    _find_buffer = key;
    _cursor.find(leaves::Slice(_find_buffer.data(), _find_buffer.size()));
  }

  void first() { _cursor.first(); }
  void last() { _cursor.last(); }
  void next() { _cursor.next(); }
  void prev() { _cursor.prev(); }
  void remove() { _cursor.remove(); }
  bool commit(bool sync) { return _cursor.commit(sync); }
  bool rollback() { return _cursor.rollback(); }
  bool start_transaction(bool non_blocking) {
    return _cursor.start_transaction(non_blocking, false);  // use_wal always false
  }
  void update() { _cursor.update(); }

  // Return the per-cursor JS context object (aspect support)
  val aspect_context() const { return _cursor.aspect_context().js_context; }

  bool is_transaction_active() const {
    return _cursor.is_transaction_active();
  }
};

// ── Factory functions ────────────────────────────────────────────
// Create a storage with a storage name.
static JSStore* make_store(const std::string& name, size_t capacity) {
  JSStore* s = new JSStore();
  s->_storage =
      std::make_shared<BrowserStoreWrapper>(name.c_str(), capacity, 0);
  return s;
}

// Close — async IDB flush. Must be be called before .delete() to avoid crash.
static void store_delete_storage(JSStore& s) {
  s._storage->_storage->delete_storage();
}

static void store_close(JSStore& s) { s._storage->_storage->destroy(); }

// Number of IDB write operations still in flight (global counter)
static int store_pending_writes() { return leaves_pending_writes(); }

// ── Open databases from a store ──────────────────────────────────
// Open a regular DB within this store.
static JSDB store_open(JSStore& s, const std::string& name) {
  DB* db = s._storage->_storage->template open<leaves::_DB>(name.c_str());
  return JSDB{s._storage, db};
}

// Open a replication DB within this store.
static ReplicationDB* store_open_replication(JSStore& s,
                                             const std::string& name) {
  // return new ReplicationDB(s._storage, name.c_str());
  try {
    return new ReplicationDB(s._storage, name.c_str());
  } catch (const std::exception& e) {
    std::cerr << "[store_open_replication] error opening replication DB: "
              << e.what() << "\n";
    throw;
  }
  return nullptr;
}

// ── Aspect callbacks on a DB handle ──────────────────────────────
// Set JS callbacks on any LeavesDB (regular or opened from replication).

static void jsdb_set_aspect_callbacks(JSDB& jsdb, val callbacks) {
  jsdb.db->aspect().set_callbacks(callbacks);
}

// Set JS callbacks on a ReplicationDB.
static void replication_db_set_aspect_callbacks(ReplicationDB& replDb, val callbacks) {
  replDb.aspect().set_callbacks(callbacks);
}

// ── Cursor functions for DB ──────────────────────────────────────
static CursorWrapper jsdb_create_cursor(JSDB& jsdb) {
  return CursorWrapper(jsdb.storage, jsdb.db);
}

static ReplicationDBCursor replication_db_create_cursor(ReplicationDB& s) {
  return ReplicationDBCursor(s.storage(), s._internal());
}

static std::vector<std::string> store_list_dbs(JSStore& s) {
  std::vector<std::string> result;
  s._storage->_storage->list_dbs(result);
  return result;
}

static val store_export(JSStore& s) {
  auto buf = s._storage->_storage->export_to_buffer();
  return val(typed_memory_view(buf.size(),
                               reinterpret_cast<const uint8_t*>(buf.data())))
      .call<val>("slice");
}

static void store_import(JSStore& s, const std::string& data) {
  std::vector<char> buf(data.begin(), data.end());
  s._storage->_storage->import_from_buffer(buf);
}

// JSTransport: a C++ implementation of ReplicationTransport that forwards calls
// to a JS object
struct JSTransport : public leaves::ReplicationTransport {
  val js_transport;

  JSTransport(val transport) : js_transport(transport) {}

  void send(const uint8_t* data, size_t size) {
    js_transport.call<void>("send", val(typed_memory_view(size, data)));
  }
};

// JSEvents: a C++ implementation of ReplicationEvents that forwards calls to a
// JS object
struct JSEvents : public leaves::ReplicationEvents {
  val js_events;

  JSEvents(val events) : js_events(events) {}

  void on_complete(uint64_t session_id, size_t nodes_transferred) override {
    if (js_events["onComplete"].as<bool>()) {
      js_events.call<void>("onComplete", session_id, nodes_transferred);
    }
  }

  void on_error(uint64_t session_id, leaves::ReplicationError error,
                const char* msg) override {
    if (js_events["onError"].as<bool>()) {
      js_events.call<void>("onError", session_id, std::string(msg));
    }
  }

  void on_progress(uint64_t session_id, size_t bytes_transferred,
                   size_t nodes_transferred) override {
    if (js_events["onProgress"].as<bool>()) {
      js_events.call<void>("onProgress", session_id, bytes_transferred,
                           nodes_transferred);
    }
  }
};

// ── ReplicationSenderJS ───────────────────────────────────────────
// Directly wraps leaves::ReplicationSender for use from JavaScript.

class ReplicationSenderJS {
 public:
  using Sender =
      leaves::ReplicationSender<BrowserStoreWrapper, leaves::_ReplicationDB>;

  ReplicationSenderJS(ReplicationDB& db) : sender_(db) {}

  void begin(val transport, val events) {
    transport_ = std::make_unique<JSTransport>(transport);
    events_ = std::make_unique<JSEvents>(events);
    sender_.begin(transport_.get(), events_.get());
  }

  void on_message_received(std::string message) {
    sender_.on_message_received(
        reinterpret_cast<const uint8_t*>(message.data()), message.size());
  }

  std::string state() const {
    switch (sender_.state()) {
      case leaves::ReplicationState::IDLE:
        return "idle";
      case leaves::ReplicationState::ACTIVE:
        return "active";
      case leaves::ReplicationState::ERROR:
        return "error";
    }
    return "unknown";
  }

 private:
  Sender sender_;
  std::unique_ptr<JSTransport> transport_;
  std::unique_ptr<JSEvents> events_;
};

// ── ReplicationReceiverJS ─────────────────────────────────────────
// Directly wraps leaves::ReplicationReceiver for use from JavaScript.

class ReplicationReceiverJS {
 public:
  using Receiver =
      leaves::ReplicationReceiver<BrowserStoreWrapper, leaves::_ReplicationDB>;

  ReplicationReceiverJS(ReplicationDB& db) : receiver_(db) {}

  void begin(val transport, val events) {
    transport_ = std::make_unique<JSTransport>(transport);
    events_ = std::make_unique<JSEvents>(events);
    receiver_.begin(transport_.get(), events_.get());
  }

  void on_message_received(std::string message) {
    auto& buffer = receiver_.receive_buffer();
    if (buffer.available() < message.size()) {
      return;
    }
    std::memcpy(buffer.write_ptr(), message.data(), message.size());
    buffer.advance(message.size());
    receiver_.on_data_received();
  }

  std::string state() const {
    switch (receiver_.state()) {
      case leaves::ReplicationState::IDLE:
        return "idle";
      case leaves::ReplicationState::ACTIVE:
        return "active";
      case leaves::ReplicationState::ERROR:
        return "error";
    }
    return "unknown";
  }

 private:
  Receiver receiver_;
  std::unique_ptr<JSTransport> transport_;
  std::unique_ptr<JSEvents> events_;
};

EMSCRIPTEN_BINDINGS(leaves) {
  register_vector<std::string>("VectorString");

  // ── Storage (JSStore) ──────────────────────────────────────────
  // A storage holds the IndexedDB connection. Use .open(dbName) to get a DB.
  //
  // All databases opened through this store support aspect callbacks.
  // Set them via LeavesDB.setAspectCallbacks(callbacksObject).
  //
  // Usage:
  //   var store = await Module.LeavesStore.create('storageName', capacity);
  //   var db    = await store.open('dbName');
  //   var c     = db.createCursor();

  class_<JSStore>("LeavesStore")
      .class_function("create", &make_store, allow_raw_pointers(), async())
      .class_function("pendingWrites", &store_pending_writes)
      .function("open", &store_open, async())
      .function("openReplication", &store_open_replication,
                allow_raw_pointers(), async())
      .function("deleteStorage", &store_delete_storage, async())
      .function("close", &store_close, async())
      .function("listDbs", &store_list_dbs)
      .function("exportToBuffer", &store_export)
      .function("importFromBuffer", &store_import, async());

  class_<ReplicationDBCursor>("LeavesReplicationDBCursor")
      .function("startTransaction", &ReplicationDBCursor::start_transaction,
                async())
      .function("find", &ReplicationDBCursor::find, async())
      .function("first", &ReplicationDBCursor::first, async())
      .function("last", &ReplicationDBCursor::last, async())
      .function("next", &ReplicationDBCursor::next, async())
      .function("prev", &ReplicationDBCursor::prev, async())
      .function("isValid", &ReplicationDBCursor::is_valid)
      .function("key", &ReplicationDBCursor::key)
      .function("getValue", &ReplicationDBCursor::get_value, async())
      .function("setValue", &ReplicationDBCursor::set_value, async())
      .function("keyBytes", &ReplicationDBCursor::key_bytes)
      .function("getValueBytes", &ReplicationDBCursor::get_value_bytes, async())
      .function("setValueBytes", &ReplicationDBCursor::set_value_bytes, async())
      .function("remove", &ReplicationDBCursor::remove, async())
      .function("commit", &ReplicationDBCursor::commit, async())
      .function("rollback", &ReplicationDBCursor::rollback, async())
      .function("isTransactionActive",
                &ReplicationDBCursor::is_transaction_active)
      .function("update", &ReplicationDBCursor::update)
      .function("aspectContext", &ReplicationDBCursor::aspect_context);

  // ── DB ─────────────────────────────────────────────────────────
  // A database handle (opened from a store). Provides createCursor()
  // and aspect callbacks.

  class_<JSDB>("LeavesDB")
      .function("setAspectCallbacks", &jsdb_set_aspect_callbacks)
      .function("createCursor", &jsdb_create_cursor, allow_raw_pointers());

  // ── ReplicationDB (returned by openReplication) ─────────────────

  class_<ReplicationDB>("ReplicationDB")
      .function("setAspectCallbacks", &replication_db_set_aspect_callbacks)
      .function("createCursor", &replication_db_create_cursor,
                allow_raw_pointers());

  // ── Replication sender/receiver ─────────────────────────────────

  class_<ReplicationSenderJS>("ReplicationSender")
      .constructor<ReplicationDB&>()
      .function("begin", &ReplicationSenderJS::begin, async())
      .function("onMessageReceived", &ReplicationSenderJS::on_message_received,
                async())
      .function("state", &ReplicationSenderJS::state);

  class_<ReplicationReceiverJS>("ReplicationReceiver")
      .constructor<ReplicationDB&>()
      .function("begin", &ReplicationReceiverJS::begin, async())
      .function("onMessageReceived",
                &ReplicationReceiverJS::on_message_received, async())
      .function("state", &ReplicationReceiverJS::state);

  // ── Cursor ─────────────────────────────────────────────────────
  // Full API including aspect context support.

  class_<CursorWrapper>("LeavesCursor")
      .function("startTransaction", &CursorWrapper::start_transaction, async())
      .function("find", &CursorWrapper::find, async())
      .function("first", &CursorWrapper::first, async())
      .function("last", &CursorWrapper::last, async())
      .function("next", &CursorWrapper::next, async())
      .function("prev", &CursorWrapper::prev, async())
      .function("isValid", &CursorWrapper::is_valid)
      .function("key", &CursorWrapper::key)
      .function("getValue", &CursorWrapper::get_value, async())
      .function("setValue", &CursorWrapper::set_value, async())
      .function("keyBytes", &CursorWrapper::key_bytes)
      .function("getValueBytes", &CursorWrapper::get_value_bytes, async())
      .function("setValueBytes", &CursorWrapper::set_value_bytes, async())
      .function("remove", &CursorWrapper::remove, async())
      .function("commit", &CursorWrapper::commit, async())
      .function("rollback", &CursorWrapper::rollback, async())
      .function("isTransactionActive", &CursorWrapper::is_transaction_active)
      .function("update", &CursorWrapper::update)
      .function("aspectContext", &CursorWrapper::aspect_context);
}

#endif  // __EMSCRIPTEN__