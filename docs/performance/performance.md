# The Fastest Key-Value Store? A Fair Fight Against LMDB

"The fastest key-value store" is a common claim in the database world.

We put nine key-value engines that once claimed to be the fastest, through a rigorous benchmark suite and compared them against the dominant embedded reference: the Lightning Memory-Mapped Database (LMDB).
There is only one that consistently outperforms LMDB across a wide range of practical workloads, while still providing ACID transactions and ordered scans.

## The contenders

The lineup covers a broad range of architectures:

| Engine | Type | Architecture |
| --- | --- | --- |
| **LMDB** | Embedded | B+ tree, memory-mapped, copy-on-write |
| **LevelDB** | Embedded | LSM tree, sorted string tables |
| **RocksDB** | Embedded | LSM tree (LevelDB fork, optimized) |
| **WiredTiger** | Embedded | B-tree / LSM hybrid (MongoDB's engine) |
| **SQLite** | Embedded | B-tree with SQL layer |
| **BadgerDB** | Embedded | LSM tree with value log (Go, via CGo) |
| **Redis** | Client/Server | In-memory hash tables |
| **DragonflyDB** | Client/Server | In-memory (Redis-compatible, multithreaded) |
| **Leaves** | Embedded | Trie-based, memory-mapped, copy-on-write |

## Early learnt lessons

Mordern key-value stores can be so fast that the slightest overhead already skews the results. Therefore for this article we focused on embedded key-value stores. For demonstration Redis, DragonflyDB, and BadgerDB were included — Redis and DragonflyDB are left behind by network round-trip cost, and BadgerDB pays a CGo boundary crossing penalty on every operation.

### Benchmark overhead

I started with the original Java YCSB and was surprised about LMDB not showing the expected performance. Some profiling revealed the problem: the benchmark framework itself was consuming a large fraction of the CPU time, especially in read-heavy workloads. That overhead was hiding the true performance differences between engines.

For a more accurate comparison, I switched to YCSB-cpp — but the pure C++ benchmark also spent most of its time in framework code, not the database. So I did some heavy optimization for lower overhead in hot paths.

#### How much overhead does a benchmark framework add?

To quantify this, we profiled the same workloads against LMDB using both the original unoptimized YCSB-cpp and our optimized version. Using Linux `perf` with per-shared-object attribution, I measured what fraction of application-level CPU time is spent inside the database library (liblmdb) versus the benchmark framework (key generation, value serialization, field construction, measurement).

| Workload | Version | DB time | Framework time | Framework overhead |
| --- | --- | --- | --- | --- |
| Analytics Read (100% read) | Original YCSB-cpp | 37.5% | 23.5% | **38.5%** |
| Analytics Read (100% read) | Optimized YCSB-cpp | 57.2% | 8.4% | **12.8%** |
| RMW (50% read, 50% RMW) | Original YCSB-cpp | 37.6% | 23.1% | **38.0%** |
| RMW (50% read, 50% RMW) | Optimized YCSB-cpp | 53.6% | 11.2% | **17.3%** |
| Batch Insert (80% insert, 15% update, 5% read) | Original YCSB-cpp | 27.8% | 25.4% | **47.8%** |
| Batch Insert (80% insert, 15% update, 5% read) | Optimized YCSB-cpp | 28.7% | 24.9% | **46.5%** |

*Framework overhead = Framework / (Framework + DB). Remaining time is system (libc, kernel, page faults).*
*RMW = read-modify-write.*

In read-heavy workloads, the unoptimized benchmark wastes nearly **40%** of its application CPU time on framework code — constructing field vectors, allocating strings, copying values — instead of measuring the database. After optimization (contiguous memory buffers, pre-allocated field names, zero-copy slices), that drops to **13%**.

The batch insert workload shows similar framework overhead in both versions because write-commit cost dominates — the framework's per-operation overhead is small compared to the disk/mmap commit.

The throughput impact is significant:

| Workload | Original YCSB-cpp | Optimized YCSB-cpp | Speedup |
| --- | --- | --- | --- |
| Analytics Read | 782,150 ops/sec | 1,129,890 ops/sec | **1.44×** |
| RMW | 402,996 ops/sec | 610,086 ops/sec | **1.51×** |
| Batch Insert | 176,398 ops/sec | 193,318 ops/sec | 1.10× |

We verified the same pattern with LevelDB: the original framework consumes 15–25% of application time in read-heavy workloads, dropping to 6–8% after optimization.

#### How overhead favors slower databases

Benchmark overhead adds a roughly constant per-operation cost. A faster database finishes its work sooner, so the overhead takes a larger share of each measured operation. A slower database spends more time in real work, making the same overhead proportionally smaller.

This compresses the apparent gap between engines. Measuring LMDB and LevelDB on the same Analytics Read workload:

| Benchmark | LMDB ops/sec | LevelDB ops/sec | LMDB / LevelDB |
| --- | --- | --- | --- |
| Original YCSB-cpp | 782,150 | 474,798 | **1.65×** |
| Optimized YCSB-cpp | 1,129,890 | 622,623 | **1.81×** |

LMDB is actually **1.81×** faster than LevelDB on this workload, but through the unoptimized benchmark it only appears **1.65×** faster. The constant framework overhead eats into LMDB's advantage more than LevelDB's, because LMDB completes its database work faster and spends proportionally more time waiting in benchmark code.

The same effect applies to every engine that is faster than the benchmark's overhead floor. The faster the engine, the more its real advantage gets hidden. All results in this article use the optimized framework to ensure the numbers reflect actual engine performance, not framework artifacts.

### 2. Best-performance configuration per engine

Each database was configured for maximum throughput, not defaults. This includes workload-level and backend-level switches such as:

- Binary keys where supported (Leaves, LMDB, LevelDB, RocksDB)
- Batch size tuning on write-heavy workloads
- Engine-specific fast-path settings (LMDB's writemap, RocksDB's compression, WiredTiger's cache)
- Scenario-specific sync/transaction settings for ACID runs

The goal was not to make one database look good, but to give each engine its strongest realistic setup.

### 3. In-process focus

The core competition is limited to in-process embedded stores, because socket or IPC boundaries can dominate latency and distort the database engine comparison. BadgerDB's CGo boundary crossing adds a similar penalty — each call from C++ into Go pays a goroutine setup and context switch cost.

Redis, DragonflyDB, and BadgerDB are shown as reference points:

- Redis and DragonflyDB demonstrate the network overhead floor
- BadgerDB shows the CGo interop cost on an otherwise competitive LSM engine
- None are directly comparable to pure in-process, pointer-level access paths

### 4. Workloads and scenarios

The benchmark matrix spans practical application patterns:

- Session state
- Cache-like lookups
- Analytics reads
- Ingest
- Latest-skewed reads
- Range 10 / Range 100 scans
- Read-modify-write
- Batch insert/update variants
- ACID A/C/I and explicit multi-key ACID transactions

## Results: The Big Bar Reveal

![Workload Comparison](../../YCSB-cpp/benchmark_graphs/workload_comparison_average.png)

Now back to the opening question.

The large bar belongs to **Leaves**.

Across the main workload suite, Leaves is consistently the top performer and clearly ahead of the classic fast embedded set, including LMDB.

The competitive landscape breaks down into clear tiers:

1. **Leaves** — first place on every workload, 1.3–1.6× ahead of LMDB on read-heavy work
2. **LMDB** — the established king of embedded key-value stores, strong across the board
3. **WiredTiger** — solid on scans (Range 10: 361K), but trails LMDB on reads
4. **LevelDB / RocksDB** — mid-tier LSM engines, respectable but well behind the B-tree/trie leaders
5. **SQLite** — surprisingly competitive on reads despite the SQL layer overhead
6. **BadgerDB** — a capable Go LSM engine, but CGo interop costs suppress throughput to 40–187K ops/sec
7. **Redis / DragonflyDB** — network-bound, 10–82K ops/sec range. DragonflyDB's multithreading does not help at single-thread benchmark scale

The most interesting result is the gap between embedded engines. LMDB at 1.2M ops/sec on Analytics Read is excellent — and Leaves still beats it by 37%.

## Why Leaves Performs So Well

Leaves combines several properties that matter for real workload throughput:

- Trie-based ordered storage with efficient navigation
- Cursor-centric API for reads, writes, and scans
- Copy-on-write internals with lock-free readers and consistent snapshots
- ACID transactions with commit/rollback and optional sync
- Multiple storage backends (memory-mapped and file-backed)
- Header-only C++20 design with low integration overhead

In short: Leaves is built like an embedded systems component, not as a wrapped client/server architecture. That shows up in mixed real-world workloads.

## Workload Scenario Breakdown

This section explains what each scenario represents, which knobs matter, and why performance behaves the way it does.

### Session (50% read / 50% update)

Simulates mutable per-user state.

Important switches:

- `*.batch_size=64` on engines that support batching
- Binary keys on supported engines

Why behavior looks like this:

- Frequent updates reward efficient update/merge paths
- Read-write alternation reduces effective batch depth
- Engines with low per-op overhead and efficient in-memory structures win

### Cache (95% read / 5% update)

Simulates metadata/profile lookup services.

Important switches:

- Read-heavy setup with zipfian access distribution

Why behavior looks like this:

- Read path efficiency dominates
- Locking, lookup depth, and key encoding overhead become decisive

### Analytics Read (100% read)

Simulates read-only feature serving.

Important switches:

- Pure read path, no write effects

Why behavior looks like this:

- This is the cleanest read-latency proxy
- High cache locality plus lightweight cursor navigation strongly helps

### Ingest (70% insert / 20% update / 10% read)

Simulates event pipelines with occasional correction updates.

Important switches:

- `insertorder=hashed`
- Batch size and binary key on supported engines

Why behavior looks like this:

- Insert/update cost and commit strategy dominate
- Engines that can absorb high mutation rates without expensive per-op sync perform best

### Latest (95% read / 5% insert)

Simulates recency-biased feeds.

Important switches:

- `requestdistribution=latest`

Why behavior looks like this:

- Hot recent keys stress pointer locality and update of near-head data
- Engines with efficient hot-set handling do well

### Range 10 / Range 100

Simulates pagination and larger window scans.

Important switches:

- Fixed scan lengths (`10` and `100`)
- Ordered access requirement

Why behavior looks like this:

- Range cost scales with scan depth and traversal efficiency
- True ordered scan implementation matters more than point lookup speed

### RMW (read-modify-write)

Simulates counters, aggregates, and document patching.

Important switches:

- 50/50 read and RMW mix

Why behavior looks like this:

- Both read and update machinery are in the hot loop
- Merge/update semantics and allocation behavior become primary bottlenecks

### Batch Insert / Batch Update Scenarios

Stress tests for write-heavy behavior under different batch sizes.

Important switches:

- `batch_size` variants: 1, 8, 32, 64

Why behavior looks like this:

- Larger batches can dramatically reduce commit overhead
- Mixed read/write phases still cap effective batching in realistic traces

### ACID A/C/I and Explicit ACID Txn

Validates correctness-oriented transaction behavior under stricter settings.

Important switches:

- Durable sync/transaction settings per engine
- Explicit multi-key transaction mode where supported

Why behavior looks like this:

- Durability and transactional guarantees add unavoidable overhead
- Relative gaps narrow, but architecture still matters

## Conclusion

If you benchmark embedded key-value engines seriously, LMDB is the baseline you must beat.

In this comparison — nine engines, eight workloads, multiple scenario variants — most engines do not.

One does.

**Leaves is the standout performer across the practical workload matrix, while still providing ACID transactions, ordered scans, and replication-ready architecture.**

The results also show why architecture matters more than marketing claims. BadgerDB is a well-regarded Go engine, but CGo boundary crossing costs make it 5–30× slower than native C/C++ embedded stores on the same workloads. Redis and DragonflyDB, despite being "fast" in-memory stores, are fundamentally limited by network round-trips when competing against in-process engines.

If your target is maximum in-process key-value throughput with modern embedded semantics, Leaves is the new reference candidate.
