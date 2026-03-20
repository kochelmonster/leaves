## Plan: Transaction Lifecycle Optimization

**TL;DR:** The main costs per `start_transaction`/`prepare_commit`/`commit` cycle are: (1) GC walk every start, (2) ~992-byte `clone()` memcpy every commit, (3) interprocess mutex overhead, (4) unnecessary prefetches. Several can be eliminated or deferred.

---

### A. Defer/Skip GC in `start_transaction` (HIGH IMPACT)

`start_transaction` acquires `txn_ref_lock` and walks the full transaction linked list (`iter_transactions`) on **every** `start_transaction` to free stale txns. In a single-cursor tight loop, this always finds exactly one txn chain of length 1-2 to free — but the locking + iteration overhead is fixed.

**Idea:** Track the chain length or skip GC when `start_txn == read_txn` (meaning no old transactions exist). Alternatively, only run GC every N transactions, or when a generation counter indicates stale txns may exist:

```cpp
// Skip GC entirely when there's only the current read_txn
if (last_txn->start_txn == resolve(last_txn)) {
    _active_txn->start_txn = resolve(last_txn);
    _start_txn_id = last_txn->txn_id;
} else {
    // existing GC logic
}
```

This eliminates the `txn_ref_lock` acquisition and linked-list walk in the common single-cursor case.

---

### B. Lighter `clone()` in `prepare_commit` (HIGH IMPACT)

`clone()` does `memcpy(sizeof(TransactionBase))` which is ~992 bytes (4 `_MemManager` × ~224B + spinlocks + atomics + fields), then `reinit_locks()` which loops over POOL_SIZE locks.

**Idea:** The pre-allocated next-txn page only needs a few fields initialized — not a full copy of the current transaction's garbage state. Instead of cloning, allocate a blank page and copy only the ~40 bytes of essential state (root, offset_root, free_bigmem_root, start_txn, area tails, txn_id). Initialize `mem_manager` fresh from the allocation frontier. This turns a 992-byte memcpy into a 40-byte field copy.

**Alternative:** "Double-buffer" — keep two transaction pages and alternate. After commit, the old read_txn page becomes the next pre-allocated page. No allocation or memcpy at all — just reinitialize the fields in-place.

---

### C. Remove Prefetch Loop in `start_transaction` (QUICK WIN)

`start_transaction` issues 7 prefetch calls for the mem_manager slots. But the `_wtxn` page was just written by `clone()` during the previous commit, so it's likely already in L1/L2 cache. These prefetches may actually be counterproductive (polluting the prefetch queue).

**Idea:** Remove or profile — if the page is already hot, these are wasted instructions.

---

### D. Single-Process Fast Mutex (MEDIUM)

`txn_lock` is `boost::interprocess::interprocess_mutex` which goes through a kernel futex path. For the common single-process case, a `SpinLock` or `std::mutex` would be significantly faster.

**Idea:** Template parameter or runtime flag: when `MAX_PROCESSES == 1` or when opened in exclusive mode, use `SpinLock` for `txn_lock`. For the benchmark, this could eliminate a syscall per transaction.

---

### E. Reduce `TransactionBase` Size (MEDIUM)

With `POOL_SIZE=4`, each `_MemManager` has 6 `_GarbageSlot` (each with 5 fields of 8+ bytes) + allocation pointers. That's 4 × ~224 = ~896 bytes of manager state alone.

**Idea:** Managers 1-3 start empty and lazily acquire areas. For `clone()`, only copy manager[0] and zero managers 1-3 with a single memset. Or separate the garbage slots from the persistent transaction state — store them out-of-band.

---

### F. Direct-Pointer GC (MEDIUM, complements A)

`iter_transactions` walks a linked list from `start_txn` to `read_txn`. With many accumulated transactions, this becomes O(N).

**Idea:** Maintain a `_header->oldest_active_txn` pointer updated during GC. Next GC starts from there instead of `start_txn`, making it O(freed) instead of O(total).

---

### G. Lock-Free Single-Writer Commit (LOW-MEDIUM)

Since there's only one writer at a time (`txn_lock` ensures this), the commit itself (`read_txn = prepared_txn`) is already a single atomic pointer store. The lock only matters for GC vs `txn_ref()` races.

**Idea:** Narrow the `txn_lock` scope to just the `start_transaction` + `end_transaction` bookkeeping, releasing it before the actual insert work. This wouldn't speed up the benchmark (lock is uncontended) but would reduce lock hold time for multi-reader scenarios.

---

### H. Defer `flush` in Commit (LOW for mmap)

`commit()` calls `flush(sync, force=true)` which triggers `msync`. For async mode (the benchmark default), this is already deferred by the kernel.

---

**Recommended order:** A → B → C → D. Items A and B together should eliminate most of the per-commit overhead. C is a quick experiment. D matters more for production than benchmarks.

**Further Considerations:**
1. For idea B (double-buffer), the old read_txn's page slot might not match `SLOT_ID` if it was upgraded — would need validation. Worth checking if transaction slots are always uniform.
2. For idea A, the single-cursor fast path needs to handle the edge case where a reader cursor holds an old txn ref (preventing GC). A generation counter or `refs` check on `last_txn` would cover this.
