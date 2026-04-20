# AI Continuation Handoff

This document is a continuation handoff for another AI agent.
It summarizes the current architecture, what changed recently, why scaling is poor, and what to do next.

## 1. Current Architecture (Relevant to Write Path)

### 1.1 Layers
- Public wrappers:
  - `include/leaves/db.hpp`
  - `include/leaves/cursor.hpp`
- Core internals:
  - `include/leaves/intern/db/_db.hpp`
  - `include/leaves/intern/db/_cursor.hpp`
- Storage backend used by benchmark path:
  - `include/leaves/intern/storage/_mmap.hpp`
- Benchmark harness:
  - `benchmarks/db_bench_leaves.cpp`

### 1.2 Transaction Model
- A transaction context (`_TxnContext`) exists per DB context.
- Context `0` is the main (publish) context.
- Contexts `1..N` are worker contexts in multithread mode.
- Cursor write lifecycle:
  1. `start_transaction()` claims a context and starts txn state.
  2. Writes mutate trie state in worker context.
  3. `commit()` prepares and either defers or flushes to main.
  4. In multithread mode, publish/visibility still flows through main context.

### 1.3 Group Merge / Commit Path
- In multithread mode, worker commits can enqueue in `_pending_merge_bitmap`.
- A leader claims context `0` and merges worker deltas in `_do_leader_merge()`.
- The leader commits/publishes the merged state, then wakes waiters.
- This is effectively a single serialized publish lane.

### 1.4 Deferred Commit State
- Deferred sequence state is now runtime-only in `_DB::_deferred_ctx` sidecar array.
- Fields include:
  - `accumulated_txn`
  - `pending_commit_count`
  - `diverge_txn_id`
  - `in_deferred_sequence`
- Important: deferred state is intentionally not stored in persisted `_TxnContext` layout.

## 2. How Architecture Evolved During This Work

## 2.1 Initial Goal
- Improve small-batch (`batch_size=10`) multithread scaling.
- Preserve read-your-own-writes semantics for deferred commits.

## 2.2 Implemented Behavioral Changes
- Added deferred threshold logic in DB internals.
- Added deferred continuation logic so a worker can continue from accumulated state.
- Added group merge behavior using pending bitmap + leader merge.
- Integrated benchmark flag `--flush_interval` to control deferred threshold.

## 2.3 Public API / Benchmark Surface Changes
- `include/leaves/db.hpp`:
  - added `set_deferred_flush_threshold(uint32_t)`
- `include/leaves/cursor.hpp`:
  - added `flush()` passthrough
- `benchmarks/db_bench_leaves.cpp`:
  - uses `set_deferred_flush_threshold(...)`
  - calls `cursor.flush()` at thread end

## 2.4 Regression and Fix
- Regression observed: many `test_cursor` failures with graph mismatch patterns.
- Root cause: deferred fields were added to persisted `_TxnContext`, affecting layout-sensitive behavior/tests.
- Fix:
  - removed deferred fields from persisted `_TxnContext`
  - moved deferred bookkeeping to runtime `_DB` sidecar state
- Result: `test_cursor` passed again after fix.

## 3. Validation Status Snapshot

### 3.1 Build / Tests
- Build succeeded after wrapper and internal fixes.
- Focus tests passed:
  - `test_db`
  - `test_multithread`
  - `test_memory`
  - `test_merger`
  - `test_cursor`
- `test_hash_updater` previously timed out under a short bounded timeout run (not proven failing, but unresolved runtime expectation).

### 3.2 Benchmark Scaling Data (Observed)

#### Batch size 10
- `flush_interval=0`
  - 1 thread: 1,724,137 ops/s
  - 2 threads: 1,718,213 ops/s (0.997x)
  - 4 threads: 1,721,170 ops/s (0.998x)
  - 8 threads: 1,739,130 ops/s (1.009x)
- `flush_interval=200`
  - 1 thread: 571,428 ops/s
  - 2 threads: 563,697 ops/s (0.986x)
  - 4 threads: 562,746 ops/s (0.985x)
  - 8 threads: 559,597 ops/s (0.979x)

#### Larger batches (100, 1000, 10000)
- `flush_interval=0` remains best across tested settings.
- Best observed speedups versus 1-thread are small:
  - batch 100: up to about 1.063x
  - batch 1000: up to about 1.036x
  - batch 10000: up to about 1.020x

#### Key format A/B (batch 100, fi=0)
- decimal key mode:
  - 1 thread: 0.505 us/op
  - 8 threads: 0.491 us/op (almost flat)
- binary key mode:
  - 1 thread: 0.298 us/op
  - 8 threads: 0.311 us/op
- Interpretation:
  - key formatting cost is real for absolute throughput
  - removing formatting overhead does not fix scaling

## 4. Findings: Why Scaling is Bad

### 4.1 Single Publish Lane in Multithread Path
- Worker deltas eventually serialize through main context commit/publish path.
- Leader-based merge reduces some coordination overhead, but merge/publish is still fundamentally serialized.

### 4.2 Conflict Pattern in Benchmark
- `fillrandom` uses a shared random keyspace across all threads.
- This creates high overlap on trie paths and increases merge work interaction.

### 4.3 Deferred Threshold Setting (as tested) Did Not Help
- `flush_interval=200` was consistently slower and did not improve scaling in measured runs.
- The tested deferred strategy appears to add overhead without reducing the dominant serialized lane enough.

### 4.4 Hotspot Profile Signals
- Profiles showed heavy time in write and memory management paths.
- No strong lock-wait/futex dominance in sampled output.
- This matches a pattern of serialized useful work (merge/publish and structural mutation), not only lock spinning.

## 5. Recommendations to Improve Scaling

## 5.1 Highest Impact Architectural Changes

1. Reduce publish frequency in multithread mode
- Coalesce more work before main-context publish.
- Merge multiple worker batches per publish cycle.
- Target: amortize single-lane merge/publish cost.

2. Lower conflict for benchmark and diagnostics
- Add benchmark mode with thread-partitioned key ranges.
- Keep existing shared-random mode too.
- Use both modes to separate engine serialization from keyspace conflict effects.

3. Make multithread benchmark use binary keys by default
- Keep decimal key mode opt-in for comparability.
- Improves absolute throughput and reduces noise from formatting overhead.

## 5.2 Secondary Optimizations

4. Reduce per-batch transaction churn
- Review clone/free frequency in commit path.
- Explore recycling strategy for transient txn pages in worker lanes.

5. Merge-path micro-optimizations
- Reduce repeated traversal and object setup in `_merge_to_main`.
- Profile with richer sampling after each change.

6. Re-evaluate deferred threshold policy
- Static threshold may not fit all workloads.
- Consider adaptive threshold based on queue depth or observed merge latency.

## 5.3 Measurement Plan (recommended next)

1. Add partitioned-key benchmark switch.
2. Run matrix:
  - threads: 1,2,4,8,16
  - batch: 10,100,1000
  - key mode: binary
  - key distribution: shared-random vs partitioned
3. Collect:
  - ops/s, us/op
  - speedup vs 1-thread
  - merge/publish counters (instrument in `_do_leader_merge`)

## 6. Suggested Instrumentation Points

- In `_DB::commit(...)`:
  - count deferred commits, forced flush commits, leader elections, follower waits
- In `_DB::_do_leader_merge(...)`:
  - number of merged worker contexts per leader cycle
  - time spent in merge loop vs publish flush
- In benchmark thread loop:
  - batches per thread, explicit flush count, total commits

## 7. Known Risks / Caveats

- Avoid modifying persisted `_TxnContext` layout again unless tests and persistence assumptions are updated accordingly.
- Keep wrapper APIs aligned with internal capabilities used by benchmarks.
- Some tests may be duration-sensitive; bounded timeouts can produce inconclusive status.

## 8. Practical Continuation Checklist

1. Implement partitioned-key mode in `db_bench_leaves.cpp`.
2. Add lightweight counters in `_db.hpp` for merge/publish path.
3. Re-run benchmark matrix and compare shared-random vs partitioned.
4. If partitioned scales but shared-random does not, prioritize conflict-reduction and merge strategy.
5. If neither scales, prioritize reducing serialized publish/merge cost per batch.

## 9. Minimal Command Set for Reproducing Current Findings

From repo root:

```bash
./build/db_bench_leaves --benchmarks=fillrandom --num=1000000 --threads=1 --batch_size=10 --flush_interval=0
./build/db_bench_leaves --benchmarks=fillrandom --num=1000000 --threads=8 --batch_size=10 --flush_interval=0
./build/db_bench_leaves --benchmarks=fillrandom --num=1000000 --threads=1 --batch_size=10 --flush_interval=200
./build/db_bench_leaves --benchmarks=fillrandom --num=1000000 --threads=8 --batch_size=10 --flush_interval=200

./build/db_bench_leaves --benchmarks=fillrandom --num=600000 --threads=1 --batch_size=100 --flush_interval=0 --binary_key=0
./build/db_bench_leaves --benchmarks=fillrandom --num=600000 --threads=8 --batch_size=100 --flush_interval=0 --binary_key=0
./build/db_bench_leaves --benchmarks=fillrandom --num=600000 --threads=1 --batch_size=100 --flush_interval=0 --binary_key=1
./build/db_bench_leaves --benchmarks=fillrandom --num=600000 --threads=8 --batch_size=100 --flush_interval=0 --binary_key=1
```

This handoff is intentionally focused on the write/commit architecture and scaling investigation.