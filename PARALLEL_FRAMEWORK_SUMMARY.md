# Parallel Execution Framework — Implementation Summary

## Project
**Repository:** `kochelmonster/leaves` — C++ header-only trie database with CMake + Ninja build system.
**Branch work:** Adding a parallel execution framework for trie algorithms.

---

## What Is Done (Phases 1–4: Framework + Memory Pool + Tests + HashUpdater)

All code compiles cleanly, all tests pass (including regenerated reference files).

### 1. `_executor.hpp` — Executor Abstraction
**File:** `include/leaves/intern/util/_executor.hpp` (complete rewrite)

- **`_InlineExecutor`** — runs work synchronously. `post(fn)` calls `fn()` immediately. `concurrency() == 1`.
- **`_PoolExecutor`** — non-owning type-erased adapter wrapping any `_ThreadPoolMixin<Derived>`. Template ctor captures `pool.submit_task` as `std::function` and `pool.pool_size()`. Only compiled when `LEAVES_HAS_THREADS == 1`.
- **`default_executor_t`** — alias: `_PoolExecutor` when threads available, `_InlineExecutor` otherwise.
- **`LEAVES_HAS_THREADS`** macro — detects Emscripten w/o pthreads, `LEAVES_SINGLE_THREADED` define. Unchanged from before.

### 2. `_task_group.hpp` — Structured Concurrency (Collect-Expand-Dispatch)
**File:** `include/leaves/intern/util/_task_group.hpp` (complete rewrite)

Two template specializations of `_TaskGroup<Executor>`:

**`_TaskGroup<_InlineExecutor>`:**
- `spawn(fn)` runs fn inline immediately, captures first exception.
- `wait()` / `wait(dispatcher)` rethrows captured exception. Dispatcher is ignored.

**`_TaskGroup<_PoolExecutor>`:**
- `spawn(fn)` stores callable in `_pending` vector (not executed yet).
- `wait()` expands + runs all inline (no dispatcher).
- `wait(dispatcher)` does collect-expand-dispatch:
  1. **BFS expand:** runs pending tasks inline; they may call `spawn()` adding children, growing the frontier until `_pending.size() >= concurrency()`.
  2. **Dispatch:** if `_pending.size() > 1`, wraps each task with try/catch + atomic outstanding counter, calls `dispatcher(wrapped_task)` for each. Blocks on condvar until all complete.
  3. **Or inline:** if single task or no dispatcher, runs inline.
- **Depth tracking:** `_depth` counter. Nested `wait()` during BFS expansion is a no-op (returns immediately), preventing infinite recursion.
- **Exception propagation:** first exception from any dispatched task is captured and rethrown after all tasks complete.

### 3. `_PooledResolver` — Re-entrant Lock Avoidance
**File:** `include/leaves/intern/memory/_memory.hpp` (added after `_MemManager`)

- **Problem:** `_GarbageSlot::push()` internally calls `resolver.alloc_slot()` and `_GarbageSlot::pop()` calls `resolver.free()`. If `resolver` is the DB, these would go back through `_MemManagerPool` and try to acquire locks that are already held → deadlock.
- **Solution:** `_PooledResolver<DB, Traits>` wraps `DB& _db` + `_MemManager<Traits>& _mgr` (the specific locked manager).
  - `alloc_slot(slot)` → `_mgr.alloc(slot, *this)` + sets txn_id + make_dirty
  - `free(page)` → `_mgr.free(page, *this)`
  - All other methods (`resolve`, `make_dirty`, `may_recycle`, `mark_for_recycle`, `alloc_single_area`) forward to `_db`.

### 4. `_MemManagerPool` — Drop-in Replacement for `_MemManager`
**File:** `include/leaves/intern/memory/_memory.hpp` (added after `_PooledResolver`)

- **`POOL_SIZE = 4`** fixed-size array of `_MemManager<Traits>` managers.
- **Per-manager atomic locks:** `std::atomic<uint32_t> _locks[4]` (try-lock via `compare_exchange_weak`).
- **Round-robin counter:** `std::atomic<uint32_t> _next` for lock selection.
- **`_count`:** active pool size (1..POOL_SIZE). Default 1.
- **Fast path:** when `_count <= 1`, all alloc/free go directly to `_managers[0]` with zero overhead (no locking, no atomics).
- **Parallel path (`_count > 1`):**
  - Round-robin try-lock across managers.
  - On lock acquired: creates `_PooledResolver(resolver, _managers[idx])`, delegates `alloc`/`free` to that manager.
  - Fallback: spin-wait on first-choice manager with `_mm_pause` / `yield`.
- **`init(start, end)`:** initializes `_managers[0]`, zeroes rest, resets locks.
- **`activate(count, resolver)`:** seeds extra managers with areas via `resolver.alloc_single_area()`.
- **`deactivate()`:** sets `_count = 1`.
- **`reinit_locks()`:** resets all atomic locks and counter (called after `clone()`/memcpy).
- **Compatibility accessors:** `slots_at(i)`, `get_allocation_start()`, `get_allocation_end()` forward to `_managers[0]` for code that reads these fields directly.
- **Static interface preserved:** `assign_slot()`, `PAGE_ID`, `MIN_PAGE_SIZE`, `MAX_PAGE_SIZE`, `COUNT`, `PAGE_SIZES`, `Slot`, `PageContainer`.
- **Persists in mmap:** all fields are hardware atomics or plain data. Same pattern as existing `std::atomic<uint32_t> refs` and `SpinLock` in mmap'd structures.

### 5. `_db.hpp` Changes
**File:** `include/leaves/intern/db/_db.hpp`

- **`_TransactionBase::MemManager`** typedef → `_MemManagerPool<Traits>` (was `_MemManager<Traits>`).
- **`_Transaction::clone()`:** adds `new_txn->mem_manager.reinit_locks()` after memcpy + atomic placement-new.
- **`_DB::activate_pool(count)`** and **`deactivate_pool()`** methods added.
- **Prefetch loop and `_garbage_statistics`:** changed `mem_manager.slots[i]` → `mem_manager.slots_at(i)`.
- **`_check.hpp`:** changed `mm.slots[i]` → `mm.slots_at(i)`.
- **`test_db.cpp`:** changed direct field access to `get_allocation_start/end()`.
- **`alloc_slot()` and `free()` in `_DB`** are UNCHANGED — they call `mem_manager.alloc()`/`mem_manager.free()` which routes through `_MemManagerPool`.

### 6. Tests
**File:** `tests/test_executor.cpp` (new, ~350 lines)

17 test cases across 6 suites:
- `inline_executor`: post_runs_synchronously, concurrency_is_one
- `pool_executor`: wraps_thread_pool_mixin, post_executes_on_worker
- `task_group_inline`: spawn_wait_basic, exception_propagation, wait_with_dispatcher_ignored
- `task_group_pool`: collect_expand_dispatch, wait_without_dispatcher_runs_inline, exception_from_dispatched_task, nested_spawn_expansion
- `mem_manager_pool`: single_thread_fast_path, activate_multiple_managers, concurrent_alloc_free, reinit_locks_after_clone
- `pooled_resolver`: routes_alloc_to_manager, routes_free_to_manager

### 7. Reference Files Regenerated
**`tests/cmpfiles/*.yaml`:** All reference YAML files regenerated because `_MemManagerPool` is larger than `_MemManager`, shifting page allocation offsets in the serialized trie graphs.

### 8. `_HashUpdater` Parallel Integration (Phase 4)
**File:** `include/leaves/intern/replication/_hash.hpp`

**Template change:** `_HashUpdater<DataDB, HashDB>` → `_HashUpdater<DataDB, HashDB, Executor = _InlineExecutor>`.
- Added `Executor* _executor` member (nullptr for inline).
- New overload: `update_hash_trie(Executor& executor, ...)` creates `_HashUpdater<DataDB, HashDB, Executor>`.
- Original `update_hash_trie(...)` unchanged — uses default `_InlineExecutor`, zero overhead.

**Parallelized methods:**
- **`sync_matching_tries()`:** `if constexpr (Executor::is_single_threaded())` gates two paths:
  - *Inline path:* unchanged sequential loop with save/restore `_key_path`.
  - *Parallel path:* spawns each trie branch into `_TaskGroup<Executor>`. Each task captures `key = _key_path` (copy) and creates a child `_HashUpdater<DataDB, HashDB>` (inline executor) for thread-safe independent execution. `tg.wait(dispatcher)` dispatches when branches ≥ concurrency.
- **`deep_copy_data_to_hash()`:** Same `if constexpr` pattern for the trie branch loop. Leaf processing remains inline in both paths.

**Key design decisions:**
- **Only trie branches spawn tasks.** Leaf-level work is always inline — leaves are fast and spawning would add overhead without benefit.
- **`_key_path` copied only in the parallel path.** The `if constexpr` gate means the inline executor path never copies `_key_path` — it uses the existing save/restore pattern. Copies happen at `tg.spawn()` time in the parallel path only.
- **Child `_HashUpdater` per task.** Each dispatched task creates its own `_HashUpdater<DataDB, HashDB>` with `_InlineExecutor` and its own `_key_path`. This ensures thread-safe independent execution without sharing mutable state across threads.
- **No nested parallelism.** Child updaters use `_InlineExecutor`, so BFS expansion doesn't grow the frontier beyond the initial branches. If branches ≥ concurrency, all are dispatched. If fewer, all run inline. Acceptable trade-off for v1.

### 9. `_ReplicationDB` Hash Memory Pool (Phase 4a)
**File:** `include/leaves/intern/replication/_replication_db.hpp`

- **`_ReplicationDBHeader::MemManager`** → `_MemManagerPool<Traits>` (was `_MemManager<Traits>`).
- **`HashDB::MemManager`** → `_MemManagerPool<Traits>`.
- **`activate_hash_pool(count)`** / **`deactivate_hash_pool()`** methods on `_ReplicationDB`: activate/deactivate the hash trie's memory manager pool for parallel allocation.
- **`HashTrieControl::reset()`** calls `hash_mem_manager.reinit_locks()` after memset to reset atomic locks.

### 10. `_PooledResolver` Fix (Phase 4a)
**File:** `include/leaves/intern/memory/_memory.hpp`

- **`_PooledResolver::alloc_slot()`:** Now uses `if constexpr (requires { _db._active_txn; })` to conditionally access `_db._active_txn->txn_id`. This avoids compilation errors when the DB type (e.g., `HashDB`) lacks an `_active_txn` field. HashDB hash trie nodes get their `txn_id` set explicitly by `_HashUpdater` after allocation.

### 11. Parallel Hash Updater Tests (Phase 4d)
**File:** `tests/test_hash_updater.cpp`

3 new test cases under `#if LEAVES_HAS_THREADS`:
- **`parallel_matches_inline_wide_trie`:** 26 keys with different first bytes → wide root trie. Compares parallel hashes against inline hashes.
- **`parallel_matches_inline_deep_trie`:** 16 keys with shared prefixes → deep trie structure. Compares parallel vs inline.
- **`parallel_incremental_update`:** Modifies + adds keys, runs parallel update, verifies hashes match inline update.

Helper code added:
- `collect_leaf_hashes()` — traverses hash trie, returns map of key→hash for leaf-level comparison.
- `struct TestPool : _ThreadPoolMixin<TestPool>` — 4-thread pool for tests.
- `run_hash_update_parallel()` — activates pool, runs `update_hash_trie` with `_PoolExecutor`, deactivates pool.

---

## Thread-Safety Analysis (mmap storage path)

| Operation | Safety | Notes |
|-----------|--------|-------|
| `resolve()` | SAFE | Pure pointer arithmetic |
| `make_dirty()` | SAFE | No-op for mmap |
| `mark_for_recycle()` | SAFE | Writes txn_id on different pages per thread |
| `may_recycle()` | SAFE | Read-only |
| `alloc_single_area()` | SAFE | Already has `std::scoped_lock(_storage.file_lock())` |
| `alloc_slot()` / `free()` | SAFE | Protected by `_MemManagerPool` per-manager locks + `_PooledResolver` |

---

## What Is NOT Done (Next Steps)

### Phase 5: Integrate `_Merger` with the Framework
**File:** `include/leaves/intern/db/_merger.hpp`

Similar pattern to `_HashUpdater` — the merge walks two tries and can parallelize across independent subtrees.

### Phase 6: Update Call Sites
- `_replication_fsm.hpp` / `_replication_db.hpp` — pass executor when calling merger.
- Hash updater call sites already have the `update_hash_trie(executor, ...)` overload available.

### Phase 7: CacheStore Thread-Safety (if needed)
- `CacheStore::resolve()` and `make_dirty()` use a shared cache map → needs mutex-wrapping for parallel use.
- Only needed if parallel algorithms are used with `CacheStore` (not mmap).

---

## Key Design Decisions (for context)

1. **Single thread pool:** `_PoolExecutor` wraps the existing `_ThreadPoolMixin` — no second pool created.
2. **Single codebase:** Same algorithm code works single-threaded and multi-threaded. `spawn()` is inline for `_InlineExecutor`, collected for `_PoolExecutor`.
3. **`_MemManagerPool` replaces `_MemManager` in `_TransactionBase`:** Not a second field — it IS the `mem_manager`. When `_count == 1` (default), zero overhead.
4. **Hardware atomics in mmap are fine:** Existing codebase already uses `std::atomic<uint32_t>` and `SpinLock` in mmap'd structures.
5. **`_PooledResolver` over modifying `_GarbageSlot`:** Keeps `_GarbageSlot` unchanged. The resolver wrapper routes alloc_slot/free to the locked manager, preventing re-entrant lock acquisition.
6. **Collect-expand-dispatch model:** `spawn()` doesn't run or dispatch immediately. `wait(dispatcher)` does BFS expansion to grow the frontier to `concurrency` tasks, then dispatches all at once. This minimizes thread pool overhead and matches trie tree structure naturally.

---

## Build & Test Commands

```bash
# Build all
cmake --build build -j

# Run all tests
for t in build/test_*; do "$t" --log_level=error; done

# Run just the new executor tests
./build/test_executor

# Regenerate reference YAML files (if page layout changes again)
# 1. Uncomment "#define GENERATE" in tests/test.hpp
# 2. Rebuild: cmake --build build -j --target test_cursor --target test_merger
# 3. Run: ./build/test_cursor && ./build/test_merger
# 4. Re-comment "// #define GENERATE" in tests/test.hpp
# 5. Rebuild and verify
```

## File Map

| File | Status | Description |
|------|--------|-------------|
| `include/leaves/intern/util/_executor.hpp` | REWRITTEN | `_InlineExecutor` + `_PoolExecutor` |
| `include/leaves/intern/util/_task_group.hpp` | REWRITTEN | Collect-expand-dispatch `_TaskGroup` |
| `include/leaves/intern/memory/_memory.hpp` | MODIFIED | Added `_PooledResolver` + `_MemManagerPool`, fixed `alloc_slot` for HashDB |
| `include/leaves/intern/replication/_hash.hpp` | MODIFIED | `_HashUpdater<DataDB, HashDB, Executor>`, parallel `sync_matching_tries` + `deep_copy_data_to_hash` |
| `include/leaves/intern/replication/_replication_db.hpp` | MODIFIED | `_MemManagerPool` for hash mem, `activate/deactivate_hash_pool()` |
| `include/leaves/intern/db/_db.hpp` | MODIFIED | `_MemManagerPool` typedef, `reinit_locks()`, `activate/deactivate_pool()`, `slots_at()` |
| `include/leaves/intern/db/_check.hpp` | MODIFIED | `slots_at()` accessor |
| `tests/test_executor.cpp` | NEW | 17 tests for the framework |
| `tests/test_hash_updater.cpp` | MODIFIED | 3 parallel hash updater tests + helpers |
| `tests/test_db.cpp` | MODIFIED | `get_allocation_start/end()` accessors |
| `tests/cmpfiles/*.yaml` | REGENERATED | New page offsets due to larger `_MemManagerPool` |
| `CMakeLists.txt` | MODIFIED | Added `test_executor` target |
