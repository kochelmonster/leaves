# Leaves C++ API

Include `<leaves/mmap.hpp>` for the core key-value API (storage, database, and cursor). Add `<leaves/confluence.hpp>` when using confluence, and `<leaves/replication.hpp>` for replication.

For an architectural overview see [docs/architecture/architecture.md](../architecture/architecture.md)

## Quick start

```cpp
#include <leaves/mmap.hpp>

int main() {
    auto storage = leaves::MapStorage::create("mydata.lvs");
    auto db = storage->open("main");
    auto cursor = db.cursor();

    // write
    cursor.find(leaves::Slice("hello"));
    cursor.value(leaves::Slice("world"));
    cursor.commit();

    // read
    cursor.find(leaves::Slice("hello"));
    if (cursor.is_valid()) {
        leaves::Slice value = cursor.value();
    }

    // scan
    for (cursor.first(); cursor.is_valid(); cursor.next()) {
        leaves::Slice key   = cursor.key();
        leaves::Slice value = cursor.value();
    }
}
```

---

## `Slice`

`Slice` is a non-owning view over a contiguous byte range. It is the fundamental type for passing keys and values through the API. `Slice` is available by including `mmap.hpp`.

- `Slice()`
  Constructs an empty view.

- `Slice(const char* str)`
  Constructs a view over a null-terminated string and computes its size with `strlen`.

- `Slice(const std::string& src)`
  Constructs a non-owning view over `src`'s internal buffer.

- `Slice(const void* data, size_t size)`
  Constructs a view over an arbitrary byte range.

- `const char* data() const`
  Returns a pointer to the first byte.

- `size_t size() const`
  Returns the byte length of the view.

- `std::string string() const`
  Returns a copied `std::string` from the view.

---

## `MapStorage` / `MapStorage_<Traits>`

`MapStorage` (an alias for `MapStorage_<MapTraits>`) manages a memory-mapped `.lvs` file and owns all databases inside it. MapStorage is multi-thread and multi-process safe (multiple processes can open the same `.lvs` file concurrently). `MapStorage` is avalable by including `mmap.hpp`.

- `static storage_ptr create(const char* path, size_t map_size = 4 * G)`
  Creates and initializes storage backed by `path`. `map_size` is the virtual-address reservation limit.

- `MapStorage_(const char* path, size_t map_size = 4 * G)`
  Direct constructor variant; prefer `create()` for shared-pointer ownership.

- `template <template <typename> class DBClass = DB, typename... Args> TDB<MapStorage_, DBClass> open(std::string_view name, Args&&... args)`
  Opens or creates a named database. `DBClass` selects the backend and `args` are forwarded to that DB class.

- `template <template <typename> class DBClass = DB> void remove(std::string_view name)`
  Removes the named database from storage.

- `void list_dbs(std::vector<std::string>& result)`
  Appends all known database names to `result`.

- `typename StorageImpl::PoolMixin& thread_pool()`
  Returns mutable access to the internal thread pool.

- `Slice filename() const`
  Returns the opened storage filename.

- `size_t file_size() const`
  Returns the current on-disk file size in bytes.

---

## `MapStorage::DB`

A lightweight handle that represents one named database inside a storage file. Obtain one via `storage->open(...)`. DB is thread-safe and can be shared across threads, but each thread should use its own `Cursor` for read/write operations. `MapStorage::DB` is available by including `mmap.hpp`.

- `Cursor cursor()`
  Returns a new `Cursor` bound to this database.

- `Slice name() const`
  Returns the database name.

- `storage_ptr storage() const`
  Returns the owning storage shared pointer.

- `auto& aspect()`
  Returns mutable access to the database aspect object.

- `const auto& aspect() const`
  Returns read-only access to the database aspect object.

- `auto txn() const`
  Returns the current transaction descriptor.

- `tid_t transaction_active() const`
  Returns the active transaction id used for crash-recovery tracking.

- `bool commit(bool sync = true)`
  Commits current database transaction state and returns `false` if no transaction is active, the commit is vetoed by an aspect hook, or the underlying transaction commit fails. Set `sync = false` to skip `fsync`.

- `bool rollback()`
  Discards changes made since the last commit and returns `false` if no transaction is active or the rollback is vetoed by an aspect hook.

- `void defrag()`
  Performs in-place compaction to reclaim fragmented storage.

- `void set_retention(uint64_t seconds)`
  Sets replication-history retention time.

---

## `Cursor`

The cursor is the workhorse of the API. Every read and write goes through a cursor. Cursors are copyable and movable; a default-constructed cursor is empty. A cursor must be used in a single thread. `Cursor` is available by including `mmap.hpp`.

- `void find(const Slice& key)`
  Seeks to `key` or to the insertion position for that key.

- `void first()`
  Moves to the smallest key.

- `void last()`
  Moves to the largest key.

- `void next()`
  Advances to the next entry.

- `void prev()`
  Moves to the previous entry.

- `bool is_valid() const`
  Returns `true` if the cursor is currently positioned on an existing record.

- `Slice key() const`
  Returns the current key as a non-owning `Slice`.

- `Slice value() const`
  Returns the current value as a non-owning `Slice`.

- `void value(const Slice& value)`
  Inserts or overwrites the record at the current position.

- `void* reserve(size_t size)`
  Reserves `size` bytes for in-place value construction and returns a writable pointer.

- `void remove()`
  Deletes the current record.

- `void update()`
  Refreshes cursor view after out-of-band mutation.

- `bool start_transaction(bool non_blocking = false, bool use_wal = false)`
  Opens a write transaction and returns `false` if the cursor already owns a transaction, an aspect hook rejects the start, or the storage layer cannot acquire a write transaction. Set `non_blocking = true` to fail instead of waiting, and `use_wal = true` for WAL semantics. By default each `value()` / `remove()` call is starts a transaction if none is active. Use `start_transaction()` to group multiple operations into a single transaction.

- `tid_t prepare_commit(bool sync = false)`
  Moves the transaction to prepared state and returns the prepared transaction id, or `0` if no transaction is active for this cursor.

- `bool commit(bool sync = false)`
  Finalizes the active transaction and returns `false` if no transaction is active, a commit hook rejects the operation, or the underlying commit fails.

- `bool rollback()`
  Discards all changes since `start_transaction()` and returns `false` if no transaction is active or a rollback hook rejects the operation.

- `bool is_transaction_active() const`
  Returns `true` while a transaction is active.

- `tid_t txn_id() const`
  Returns the transaction id associated with the cursor read snapshot.

- `auto& aspect_context()`
  Returns mutable cursor-level aspect context.

- `const auto& aspect_context() const`
  Returns read-only cursor-level aspect context.

---

## Confluence API

Confluence is a multi-writer layer. Every `ConfluenceCursor` writes to its own tributary database, which is merged to the main database automatically if certain thresholds are met. Confluence is available by including `confluence.hpp`. See [examples/confluence_multithread](../../examples/confluence_multithread) for a complete multi-threaded demo.


#### Opening and removing a Confluence database

```cpp
auto storage = leaves::MapStorage::create("data.lvs");

// Open or create Confluence database "events".
auto cdb = storage->open<leaves::MapStorage::ConfluenceDB>("events");

// Open or create replication-enabled Confluence database "events_repl".
auto rcdb =
  storage->open<leaves::MapStorage::ConfluenceReplicationDB>("events_repl");

// Remove Confluence database "events" when using the default DB backend.
storage->remove<leaves::MapStorage::ConfluenceDB>("events");

// If ConfluenceDB is configured with ReplicationDB, remove with ReplicationDB.
storage->remove<leaves::MapStorage::ConfluenceReplicationDB>("events_repl");
```

Custom conflict policy example:

```cpp
struct MyPolicy {
  struct _Candidate {
    tid_t txn_id;
    Slice value;
    bool is_deleted;
  };

  int resolve(const Slice&, const std::vector<_Candidate>& candidates) const {
    return candidates.empty() ? -1 : 0;
  }
};

auto custom = storage->open<leaves::MapStorage::ConfluenceDB_<MyPolicy>>(
  "events_custom");
```

### `MapStorage::ConfluenceDB`

- `MapStorage::ConfluenceDB()`
  Opens or creates the named confluence database and manages main plus tributary databases.

- `ConfluenceCursor cursor()`
  Returns a `ConfluenceCursor` bound to this database.

- `void set_merge_write_threshold(uint32_t n)`
  Sets the tributary write-count threshold for merge eligibility. The tributary is merged to the main db when its write count exceeds this threshold.  

- `void set_max_attached_age_ms(uint64_t ms)`
  Sets the max attach age before a tributary becomes merge-eligible. If a tributary has not been merged for `ms` milliseconds, it is merged to the main db.

- `void merge_eligible_tributaries()`
  Merges tributaries that currently meet merge criteria.

- `void merge_now()`
  Performs synchronous merge of threshold- or idle-eligible tributaries.

- `void merge_all_now()`
  Forces merge of all free tributaries.

- `std::exception_ptr get_merge_error()`
  Returns and clears the last asynchronous merge error.

### `ConfluenceCursor`

Multiple ConfluenceCursors can write simultaneously to the database. But each cursor must be used in one thread.

- `bool start_transaction(bool non_blocking = false)`
  Starts a write transaction on this cursor's tributary and returns `false` if
  the slot cannot be claimed, another transaction is already active, or the
  tributary cannot open a write transaction.

- `bool commit(bool sync = false)`
  Commits the active transaction and returns `false` if no transaction is
  active or the tributary commit fails.

- `bool rollback()`
  Rolls back the active transaction and returns `false` if no transaction is
  active or the tributary rollback fails.

- `bool is_transaction_active() const`
  Returns `true` while a transaction is active.

- `void find(const Slice& key)`
  Seeks to `key`.

- `void value(const Slice& v)`
  Inserts or updates value at current position.

- `void remove()`
  Deletes current record.

- `bool first()`
  Moves to first key and returns validity.

- `bool next()`
  Moves to next key and returns validity.

- `bool last()`
  Moves to last key and returns validity.

- `bool prev()`
  Moves to previous key and returns validity.

- `bool is_valid() const`
  Returns current cursor validity.

- `Slice key() const`
  Returns current key.

- `Slice value() const`
  Returns current value.

---

## Replication API

Include `<leaves/replication.hpp>`. An explicit example how to use the Replication API is provided in the folder `examples/p2p_kv`.
For a high-level overview see [docs/replication/replication.md](../replication/replication.md)

### Opening and removing a ReplicationDB database

```cpp
auto storage = leaves::MapStorage::create("data.lvs");

// Open or create replication-enabled database "repl".
auto rdb = storage->open<leaves::MapStore::ReplicationDB>("repl");

// Remove replication-enabled database "repl".
storage->remove<leaves::MapStore::ReplicationDB>("repl");
```

### `ReplicationState`

- `enum class ReplicationState { IDLE, ACTIVE, ERROR };`
  `IDLE` means not started or completed, `ACTIVE` means session in progress, and `ERROR` means session failure.

### `ReplicationSender<Storage, DBClass>`

- `explicit ReplicationSender(DB& db)`
  Constructs a sender bound to source replication database `db`.

- `void begin(ReplicationTransport* transport, ReplicationEvents* events, DbType db_type = DbType::DB_MAIN)`
  Starts replication using `transport` and optional `events`, with selected DB type.

- `void on_message_received(const uint8_t* data, size_t size)`
  Feeds receiver responses into the sender FSM.

- `ReplicationState state() const`
  Returns current FSM state.

- `uint64_t session_id() const`
  Returns current session identifier.

- `ReplicationError error() const`
  Returns the last replication error code.

- `size_t bytes_transferred() const`
  Returns transferred bytes in current session.

- `size_t nodes_transferred() const`
  Returns transferred node count in current session.

- `std::chrono::steady_clock::time_point last_activity() const`
  Returns timestamp of last FSM activity.

### `ReplicationReceiver<Storage, DBClass>`

- `explicit ReplicationReceiver(DB& db)`
  Constructs a receiver bound to destination replication database `db`.

- `void begin(ReplicationTransport* transport, ReplicationEvents* events)`
  Starts a receiver session with transport and optional event callbacks.

- `ReceiveBuffer& receive_buffer()`
  Returns mutable internal receive buffer for zero-copy ingest.

- `bool on_data_received()`
  Processes newly received buffer data and returns `true` if a full message was handled.

- `ReplicationState state() const`
  Returns current FSM state.

- `uint64_t session_id() const`
  Returns current session identifier.

- `ReplicationError error() const`
  Returns the last replication error code.

- `size_t bytes_transferred() const`
  Returns transferred bytes in current session.

- `size_t nodes_transferred() const`
  Returns transferred node count in current session.

- `std::chrono::steady_clock::time_point last_activity() const`
  Returns timestamp of last FSM activity.

### `run_replication`

- `template <typename Sender, typename Receiver, typename Transport> void run_replication(Sender& sender, Receiver& receiver, Transport& sender_transport, Transport& receiver_transport, int max_rounds = 1000)`
  Runs an in-process replication loop until both sides finish, no progress remains, or `max_rounds` is reached.

### Wiring example

```cpp
#include <deque>
#include <iostream>
#include <vector>
#include <leaves/mmap.hpp>
#include <leaves/replication.hpp>

struct QueueTransport : leaves::ReplicationTransport {
  std::deque<std::vector<uint8_t>>* out;

  explicit QueueTransport(std::deque<std::vector<uint8_t>>* q) : out(q) {}

  void send(const uint8_t* data, size_t size) override {
    out->emplace_back(data, data + size);
  }
};

struct PrintEvents : leaves::ReplicationEvents {
  void on_complete(uint64_t txn_id, size_t bytes) override {
    std::cout << "replication complete: txn_id=" << txn_id
              << ", bytes=" << bytes << "\n";
  }

  void on_error(uint64_t txn_id, leaves::ReplicationError err, const char* msg) override {
    std::cerr << "replication error: txn_id=" << txn_id
              << ", code=" << static_cast<int>(err)
              << ", message=" << (msg ? msg : "") << "\n";
  }

  void on_progress(uint64_t txn_id, size_t current, size_t total) override {
    std::cout << "replication progress: txn_id=" << txn_id
              << ", bytes=" << current << "/" << total << "\n";
  }
};

int main() {
  auto src_storage = leaves::MapStorage::create("src.lvs");
  auto dst_storage = leaves::MapStorage::create("dst.lvs");

  auto src_db = src_storage->open<leaves::ReplicationDB>("main");
  auto dst_db = dst_storage->open<leaves::ReplicationDB>("main");

  std::deque<std::vector<uint8_t>> to_receiver;
  std::deque<std::vector<uint8_t>> to_sender;
  QueueTransport sender_transport(&to_receiver);
  QueueTransport receiver_transport(&to_sender);
  PrintEvents events;

  leaves::ReplicationSender<leaves::MapStorage> sender(src_db);
  leaves::ReplicationReceiver<leaves::MapStorage> receiver(dst_db);

  sender.begin(&sender_transport, &events, leaves::DbType::DB_MAIN);
  receiver.begin(&receiver_transport, &events);

  leaves::run_replication(sender, receiver, sender_transport, receiver_transport);
  return 0;
}
```

---

## Metrics

Include `<leaves/metrics.hpp>`, then instantiate storage with metrics-enabled traits:

```cpp
#include <iostream>
#include <leaves/mmap.hpp>
#include <leaves/metrics.hpp>

using MetricsMapStorage = leaves::MapStorage_<leaves::MetricsMapTraits>;
```

`MetricsMapTraits` enables all metric aspects for map-backed storage. For file-backed storage, use `leaves::MetricsFileTraits` with `FileStorage_`.

### Example 1: collect operation, transaction, and navigation metrics

```cpp
#include <iostream>
#include <leaves/mmap.hpp>
#include <leaves/metrics.hpp>

int main() {
  using Storage = leaves::MapStorage_<leaves::MetricsMapTraits>;

  auto storage = Storage::create("metrics.lvs");
  auto db = storage->open("main");
  auto cursor = db.cursor();

  // Write two keys.
  cursor.find(leaves::Slice("a"));
  cursor.value(leaves::Slice("value-a"));
  cursor.commit();

  cursor.find(leaves::Slice("b"));
  cursor.value(leaves::Slice("value-b"));
  cursor.commit();

  // Read one key.
  cursor.find(leaves::Slice("a"));
  if (cursor.is_valid()) {
    (void)cursor.value();
  }

  // Navigate.
  cursor.first();
  if (cursor.is_valid()) {
    cursor.next();
    cursor.prev();
  }

  // Delete one key.
  cursor.find(leaves::Slice("b"));
  if (cursor.is_valid()) {
    cursor.remove();
    cursor.commit();
  }

  const auto& aspect = db.aspect();
  auto ops = aspect.ops_snapshot();
  auto txns = aspect.txn_snapshot();
  auto nav = aspect.nav_snapshot();

  std::cout << "writes=" << ops.writes
            << ", reads=" << ops.reads
            << ", deletes=" << ops.deletes << "\n";
  std::cout << "user_txns_started=" << txns.user_txns_started
            << ", user_txns_committed=" << txns.user_txns_committed << "\n";
  std::cout << "finds=" << nav.finds
            << ", finds_hit=" << nav.finds_hit
            << ", navigations=" << nav.navigations << "\n";
}
```

### Example 2: observe rollback in transaction metrics

```cpp
#include <iostream>
#include <leaves/mmap.hpp>
#include <leaves/metrics.hpp>

int main() {
  using Storage = leaves::MapStorage_<leaves::MetricsMapTraits>;

  auto storage = Storage::create("metrics_txn.lvs");
  auto db = storage->open("main");
  auto cursor = db.cursor();

  // Start a user transaction and roll it back.
  cursor.start_transaction();
  cursor.find(leaves::Slice("k"));
  cursor.value(leaves::Slice("v"));
  cursor.rollback();

  // Start a new transaction and commit it.
  cursor.start_transaction();
  cursor.find(leaves::Slice("k"));
  cursor.value(leaves::Slice("v2"));
  cursor.commit();

  auto txns = db.aspect().txn_snapshot();
  std::cout << "started=" << txns.user_txns_started
            << ", committed=" << txns.user_txns_committed
            << ", rolled_back=" << txns.user_txns_rolled_back << "\n";
}
```

### Available metrics

Read metrics from `db.aspect()` through snapshot helpers:

- `OperationSnapshot ops_snapshot()`
  - `writes`: successful write operations.
  - `bytes_written`: total bytes returned by write hook.
  - `reads`: read operations through value access.
  - `bytes_read`: total bytes returned by read hook.
  - `deletes`: successful delete operations.

- `TransactionSnapshot txn_snapshot()`
  - `user_txns_started`: user-origin transactions started.
  - `user_txns_committed`: user-origin transactions committed.
  - `merge_txns_committed`: merge-origin transactions committed.
  - `defrag_txns_committed`: defrag-origin transactions committed.
  - `user_txns_rolled_back`: user-origin transactions rolled back.

- `NavigationSnapshot nav_snapshot()`
  - `finds`: total `find(...)` calls.
  - `finds_hit`: `find(...)` calls that found a key.
  - `navigations`: total `next()` + `prev()` calls.

- `MaintenanceSnapshot maintenance_snapshot()`
  - `sanitize_count`: sanitize operations.
  - `defrag_count`: defrag operations.
  - `reset_count`: reset operations.

- `MergeSnapshot merge_snapshot()`
  - `merge_overwrites`: applied merge overwrites.
  - `merge_adds`: applied merge inserts.
  - `merge_deletes`: applied merge deletes.

### Notes on when counters change

- Merge counters (`merge_*`) are driven by merge/apply hooks, so they typically change during merge or replication-style apply flows, not ordinary point writes.
- Maintenance counters increase when sanitize/defrag/reset paths run.
- Metrics are exposed via the existing aspect mechanism; there is no separate metrics service API.

