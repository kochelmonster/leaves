# Parallel Execution Framework — Implementation Summary

## Project
**Repository:** `kochelmonster/leaves` — C++ header-only trie database with CMake + Ninja build system.
**Branch work:** Adding a parallel execution framework for trie algorithms.

---

## What Is Done (Phases 1–3: Framework + Memory Pool + Tests)

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

### Phase 4: Integrate `_HashUpdater` with the Framework
**File:** `include/leaves/intern/db/_hash_updater.hpp`

The hash updater walks the trie tree computing hashes. It already has a recursive structure where each branch node can be processed independently. The integration would:
1. Accept an executor parameter (defaulting to `_InlineExecutor` for backward compat).
2. Create a `_TaskGroup` and `spawn()` each child branch during tree traversal.
3. Call `wait(dispatcher)` where dispatcher calls `executor.post()`.
4. Call `activate_pool()` before parallel work, `deactivate_pool()` after.
5. The `_PooledResolver` + `_MemManagerPool` handle thread-safe memory allocation automatically.

### Phase 5: Integrate `_Merger` with the Framework
**File:** `include/leaves/intern/db/_merger.hpp`

Similar pattern to `_HashUpdater` — the merge walks two tries and can parallelize across independent subtrees.

### Phase 6: Update Call Sites
- `_replication_fsm.hpp` / `_replication_db.hpp` — pass executor when calling hash updater / merger.

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
| `include/leaves/intern/memory/_memory.hpp` | MODIFIED | Added `_PooledResolver` + `_MemManagerPool` |
| `include/leaves/intern/db/_db.hpp` | MODIFIED | `_MemManagerPool` typedef, `reinit_locks()`, `activate/deactivate_pool()`, `slots_at()` |
| `include/leaves/intern/db/_check.hpp` | MODIFIED | `slots_at()` accessor |
| `tests/test_executor.cpp` | NEW | 17 tests for the framework |
| `tests/test_db.cpp` | MODIFIED | `get_allocation_start/end()` accessors |
| `tests/cmpfiles/*.yaml` | REGENERATED | New page offsets due to larger `_MemManagerPool` |
| `CMakeLists.txt` | MODIFIED | Added `test_executor` target |
