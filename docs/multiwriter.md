# Multiwriter Database Design Document

Complete architectural specification for recreating `_multiwriter_db.hpp` from scratch.

## Overview

The multiwriter DB enables N concurrent writers sharing a single committed trie through merge-on-conflict. Each writer has its own memory manager and transaction space; commits are serialized through a global lock. When a writer commits, if the committed state changed since its snapshot, a trie merger resolves conflicts (last-writer-wins per key).

File: `include/leaves/intern/db/_multiwriter_db.hpp`
Includes: `_db.hpp`, `_merger.hpp`, `<vector>`
Namespace: `leaves`

## Type Tag

```cpp
static constexpr uint16_t DB_TYPE_MULTIWRITER = 2;
```

## Header Structures

### _MultiWriterDBHeader<Storage_> : _BaseDBHeader

Persisted header for the multi-writer wrapper. Located at the start of a dedicated area.

Fields:
- `offset_t shared_read_txn` — Single source of truth for committed root. All readers/writers resolve through this.
- `SpinLock commit_lock` — Serializes merge-on-conflict commits across all writers.
- `SpinLock txn_ref_lock` — Shared lock for GC safety. Prevents concurrent refs increment and free.
- `std::atomic<uint32_t> next_txn_id` — Monotonically increasing txn_id source. Initialized to 2 (txn_id=1 reserved for initial shared txn).
- `uint16_t num_writers` — Persisted writer count.
- Variable-length array of `offset_t` follows immediately after struct (writer header offsets).
- `writer_offsets()` method returns pointer to this array via `reinterpret_cast<offset_t*>((char*)this + sizeof(*this))`.

### _WriterDBHeader<Storage_> : _BaseDBHeader

Per-writer persisted header. Each writer has its own.

Fields:
- `offset_t read_txn` — Local copy, NOT used at runtime (redirected to parent's shared_read_txn via method override).
- `offset_t prepared_txn` — Writer's prepared/in-flight transaction offset.
- `offset_t next_txn_page` — Pre-allocated transaction page (claimed via CAS at commit).
- `Mutex txn_lock` — Per-writer transaction lock. No cross-writer contention during writes.
- `std::atomic<uint64_t> txn_cursor_id` — ID of cursor holding active transaction.
- `SpinLock txn_ref_lock` — Local copy, NOT used (redirected to parent).

## _WriterMergePolicy<DB>

Merge policy for conflict resolution. Controls how writer's trie is merged into committed trie.

Template param: `DB` = `_WriterDB<Storage_>`

Static constants:
- `adopt_src = true` — Reuse src nodes directly (same storage, no copy needed).
- `skip_free_dst = true` — Don't free dst nodes (concurrent readers may still reference them).

Fields:
- `DB* db`
- `tid_t base_txn_id` — The snapshot txn_id at transaction start.
- `CursorContext _merge_context` — `[[no_unique_address]]` aspect context for merge callbacks.

Key method — `should_descend_src(page_ptr page)`:
- Returns `page->txn_id > base_txn_id`.
- Pages with txn_id <= base_txn_id were NOT modified by this writer (they came from the cloned committed state). The committed trie already has the correct (possibly newer) version → skip.
- Pages with txn_id > base_txn_id were allocated/modified by this writer → descend and merge.

Other methods:
- `may_overwrite()` → delegates to `db->aspect().may_merge_overwrite(...)`.
- `may_add_leaf()` → delegates to `db->aspect().may_merge_add(...)`.
- `free_rejected_src(page_ptr)` → `db->free(page)`.
- `migrate_big_value()` → returns `{Slice(leaf.vdata(), leaf.vsize()), leaf.is_big()}` (no migration needed, same storage).

## _WriterDB<Storage_>

Per-writer database. Full `_DB` subclass with its own mem_manager, garbage slots, area chains, and transaction page.

### Class hierarchy

```
_DB<Storage_, _Transaction<Traits>, _WriterDBHeader<Storage_>, _WriterDB<Storage_>>
  └── _WriterDB<Storage_>
```

CRTP: `Self_` = `_WriterDB<Storage_>` (non-void), so base `_DB` skips init/sanitize.

### Type aliases

- `Base` = the _DB instantiation above
- `Transaction`, `Traits`, `MemManager`, `page_ptr`, `txn_ptr`, `offset_e`, `area_ptr`, `Aspect` — from Base
- `MW` = `_MultiWriterDB<Storage_, void>`
- `MWHeader` = `_MultiWriterDBHeader<Storage_>`
- `CursorTraits` — extends `Base::CursorTraits` with `DB = _WriterDB`
- `Cursor` = `_TransactionalCursor<CursorTraits>`
- `RawCursor` = `_Cursor<CursorTraits>`
- `type_tag = DB_TYPE_MULTIWRITER`

### Fields

- `MW* _parent` — Pointer to owning `_MultiWriterDB`.
- `MWHeader* _mw_header` — Pointer to shared MW header (raw pointer, not smart pointer).
- `txn_ptr _snapshot_txn` — Holds ref on the committed txn snapshot during a transaction. Used for merge detection and `should_descend_src` base_txn_id. Released in commit/rollback.

### Constructor

```cpp
_WriterDB(Storage& storage, MW* parent, MWHeader* mw_header, offset_t header_offset)
    : Base(storage, header_offset, 0),
      _parent(parent), _mw_header(mw_header) {}
```

Uses `_DB(Storage&, offset_t, uint16_t)` — resolves header, sets `_active_txn` if `prepared_txn != read_txn`.

### init_writer(offset_t header_offset, offset_t area_offset, area_ptr first_area)

Called once during `_MultiWriterDB::_init()`. Sets up:
1. Resolves and zeroes the header, placement-new the txn_lock.
2. Sets `area_list_head_single = area_offset`, `area_list_head_multi = 0`.
3. Sets `read_txn = shared_read_txn` (consistency, not used at runtime).
4. Creates writer's first transaction page at `header_offset + padding(sizeof(_WriterDBHeader), MIN_PAGE_SIZE)`.
5. Sets both `prepared_txn` and `read_txn` to this txn offset.
6. Initializes the txn: zeroes, slot_id, used, txn_id from `next_txn_id`, root=0, refs=0, start_txn self-referential, area tails, mem_manager.
7. Pre-allocates next_txn_page via clone: `_active_txn = txn; next = txn->clone(*this); _header->next_txn_page = resolve(next); _active_txn.reset()`.
8. Dirty + flush.

### Method overrides (redirect to parent shared state)

**txn()** — Returns committed read transaction:
```cpp
txn_ptr txn() const {
  return this->template resolve<Transaction>(&_mw_header->shared_read_txn);
}
```

**iter_transactions(T caller)** — Walk shared committed chain:
```cpp
txn_ptr t = _storage.resolve(&_mw_header->shared_read_txn);
tid_t end = t->txn_id;
offset_t* link = &t->start_txn;
do {
  t = resolve<Transaction>(link);
  link = &t->next_txn;
  if (caller(t)) break;
} while (t->txn_id < end);
```

**txn_ref()** — Acquire snapshot reference under shared lock:
```cpp
std::lock_guard<SpinLock> guard(_mw_header->txn_ref_lock);
txn_ptr t = txn();
t->refs.fetch_add(1);
return t;
```

### start_transaction(cursor_id, nonblocking, origin)

1. Lock writer's `_header->txn_lock` (try_lock if nonblocking).
2. Assert no active txn, store cursor_id.
3. Acquire snapshot under `txn_ref_lock`: resolve `shared_read_txn`, `refs.fetch_add(1)`. Save as `_snapshot_txn`.
4. Claim pre-allocated page: `_active_txn = resolve(&_header->next_txn_page); _header->next_txn_page = 0`.
5. Assign unique txn_id: `next_txn_id.fetch_add(1)`, set `next_txn = 0`.
6. Clone committed root: copy `root`, `offset_root`, `free_bigmem_root` from snapshot.
7. GC walk under `txn_ref_lock`:
   - Set `_start_txn_id = last_txn->txn_id`.
   - Walk `iter_transactions`: if `refs > 0` → set `_active_txn->start_txn` and `_start_txn_id`, stop. If `refs == 0` → advance `shared->start_txn = txn->next_txn` THEN `free(txn)`.
   - The advance-before-free prevents double-free: other writers' GC walks (also under `txn_ref_lock`) will skip already-freed pages.
8. Return `_active_txn`.

### alloc_slot(uint16_t slot) — CAS-based page ownership

Multiple writers clone from the same committed transaction, inheriting shared garbage queues. Without CAS, two writers could claim the same recycled page.

```
for (;;) {
  result = _active_txn->mem_manager.alloc(slot, *this);
  old_tid = result->txn_id._value;
  if (old_tid == 0) { claim fresh page, return; }
  if (tid_t(old_tid) >= _start_txn_id) continue; // belongs to committed gen or other writer
  CAS(result->txn_id._value, old_tid, my_txn_id._value);
  if CAS succeeded → return; else continue;
}
```

Uses `std::atomic_ref<uint32_t>` for the CAS.

### alloc_page / alloc_node

Shadow base methods to route through our `alloc_slot`:
- `alloc_page(space)` → `alloc_slot(assign_slot(space + PageHeader))`, sets `used`.
- `alloc_node<NodePtr>(node_size)` → `alloc_page(node_size) + sizeof(PageHeader)`.

### commit(cursor_id, sync, origin)

1. Verify cursor_id matches.
2. Lock `_mw_header->commit_lock`.
3. Check merge: `current_read->txn_id != _snapshot_txn->txn_id` → needs merge.
4. If merge needed → `_merge_writer_into_committed(current_read)`.
5. Pre-allocate next_txn_page: `alloc_slot(Transaction::SLOT_ID)`, memcpy active_txn, reinit locks/refs.
6. Set `_header->prepared_txn = resolve(_active_txn)`.
7. Link into chain: `active_read->next_txn = prepared_txn`.
8. Atomic switch: `_mw_header->shared_read_txn = prepared_txn` (8-byte aligned store, hardware-atomic on x86_64).
9. Dirty + flush.
10. Release snapshot: `_snapshot_txn->refs.fetch_sub(1); _snapshot_txn.reset()`.
11. `end_transaction()`.

### rollback(cursor_id, origin)

1. Release snapshot: `_snapshot_txn->refs.fetch_sub(1); _snapshot_txn.reset()`.
2. Delegate to `Base::rollback()`.

### _merge_writer_into_committed(committed_txn)

1. Save writer_root = `_active_txn->root`.
2. Set `_active_txn->root = committed_txn->root` (destination = current committed).
3. Create `DstCursor(this, &_active_txn->root)`, rebind to writer's txn via `_set_txn()`.
4. Create `SrcCursor(this, &src_root)`, clear.
5. Create `_WriterMergePolicy(this, _snapshot_txn->txn_id)`.
6. Init merge context: `_aspect.init_cursor_context(policy._merge_context)`.
7. Execute: `_Merger(dst_cursor, src_cursor, policy).exec()`.

### create_cursor()

Creates `Cursor(this, &txn()->root)` with initialized aspect context.

### sanitize_writer()

Called after crash recovery:
1. Placement-new `txn_lock`, zero `txn_cursor_id`.
2. Walk `iter_transactions`: zero all refs, reinit mem_manager locks.
3. If `next_txn_page` exists: zero refs, reinit locks. Else: re-create from committed root via clone.
4. Dirty + flush.

## _MultiWriterDB<Storage_, Self_>

Wrapper owning N `_WriterDB` instances.

### Type aliases

- `MWHeader`, `Writer = _WriterDB<Storage_>`, `Transaction`, `txn_ptr`
- `SlotInfo = typename Storage::SlotInfo`
- `type_tag = DB_TYPE_MULTIWRITER`

### Fields

- `Storage& _storage`
- `Pointer<MWHeader> _mw_header` — Smart/simple pointer depending on storage backend.
- `uint16_t _index`
- `std::vector<std::unique_ptr<Writer>> _writers`

### Constructor

```cpp
_MultiWriterDB(Storage& storage, SlotInfo slot, uint16_t num_writers = 4)
```

- If `slot.is_new` → `_init(slot.offset_ptr, num_writers)`.
- If `slot.needs_sanitize` → resolve header, `_sanitize()`.
- Else → resolve header, `_reconstruct_writers()`.

### _init(offset_t* slot_offset, uint16_t num_writers)

1. Alloc MW area, set `*slot_offset`, resolve header.
2. Zero header, placement-new locks, `next_txn_id = 2`, set num_writers.
3. Set `area_list_head_single = mw_area_offset`.
4. Compute `mw_header_size = padding(sizeof(MWHeader) + num_writers * sizeof(offset_t), PAGE_SIZES[0])`.
5. Create shared initial transaction at `*slot_offset + mw_header_size`:
   - txn_id=1, empty roots, refs=0, start_txn=self, area tails=mw_area, mem_manager initialized.
6. Dirty + flush MW header.
7. For each writer i:
   - Alloc writer area.
   - Set `writer_offsets()[i] = writer_header_offset`.
   - Create `_WriterDB(storage, this, &*_mw_header, writer_header_offset)`.
   - Call `init_writer(header_offset, area_offset, area)`.
8. Dirty + flush MW header again.

### _reconstruct_writers()

For each `i < num_writers`: resolve `writer_offsets()[i]`, create `_WriterDB(storage, this, &*_mw_header, offset)`.

### _sanitize()

1. Placement-new `commit_lock`, `txn_ref_lock`. Dirty + flush.
2. `_reconstruct_writers()`.
3. For each writer: `sanitize_writer()`.

### Public methods

- `writer(index)` — Returns `*_writers[index]`.
- `num_writers()` — Returns `_mw_header->num_writers`.
- `is_active()` — Any writer has active transaction.
- `txn()` — Resolve `shared_read_txn`.
- `name()` — `_storage.db_name(_index)`.
- `resolve()` overloads — delegate to storage.

### Alias

```cpp
template <typename Storage_>
using _MultiWriterDefaultDB = _MultiWriterDB<Storage_>;
```

## Synchronization Summary

| Lock | Scope | Where acquired | Purpose |
|------|-------|----------------|---------|
| `_WriterDBHeader::txn_lock` | Per-writer | `start_transaction` | Serialize start/commit for one writer |
| `_MWHeader::commit_lock` | Global | `commit` | Serialize merge-on-conflict across all writers |
| `_MWHeader::txn_ref_lock` | Global | `start_transaction` (2x), `txn_ref` | GC safety: prevents concurrent free+resolve race |
| `page->txn_id` CAS | Per-page | `alloc_slot` | Ownership claim for recycled pages |

Lock ordering: `txn_lock` → `commit_lock` → `txn_ref_lock`.

## Key Invariants

1. `shared_read_txn` always points to a valid, complete committed transaction.
2. All writers' `txn()`, `iter_transactions()`, `txn_ref()` route through `_mw_header->shared_read_txn`.
3. `_snapshot_txn` holds a ref (+1) for the duration of a transaction, preventing GC.
4. GC advances `shared->start_txn` BEFORE freeing, preventing double-free across writers.
5. CAS on `page->txn_id` prevents double-allocation of recycled pages shared via cloned garbage queues.
6. The 8-byte aligned store of `shared_read_txn` is hardware-atomic on x86_64 — readers always see a consistent offset.
7. `merge-on-conflict` skips subtrees where `page->txn_id <= snapshot_txn_id` (unchanged by this writer).

## Dependencies

- `_DB<Storage_, Transaction_, Header_, Self_>` — base class providing transaction management, mem_manager, COW trie operations, area management, flush/dirty tracking.
- `_Transaction<Traits>` — transaction page with root, refs, start_txn, next_txn, mem_manager, area tails.
- `_Merger<DstCursor, SrcCursor, MergePolicy>` — trie merger with `selective_deep_copy_subtree`, `should_descend_src` filtering.
- `_TransactionalCursor` / `_Cursor` — cursor types for navigating/modifying trie.
- `_BaseDBHeader` — base header with `area_list_head_single`, `area_list_head_multi` (for crash recovery area scanning).
- `SpinLock` — hardware-only spinlock (no kernel state, safe in shared memory/mmap).
- `Storage::SlotInfo` — `{index, offset_ptr, is_new, needs_sanitize}` for open/create.

## Usage Pattern (from tests)

```cpp
auto [storage, db] = open_mw_db(num_writers);
auto& w = MW(db)->writer(i);
auto cursor = w.create_cursor();
cursor->start_transaction();
cursor->find("key")->value("val");
cursor->commit();
// Read from any writer's cursor after commit
```

`MW(db)` casts the type-erased `_DBSlot` to `_MultiWriterDB*`.
