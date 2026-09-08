# A New Concept for Key-Value Stores

Most embedded key-value stores are built around a small family of storage
structures: B-trees, B+ trees, LSM trees, and _hash tables. Tries are usually
reserved for routing tables, dictionaries, autocomplete, and specialized
string indexes.

This article examines whether a suitable trie variant can outperform
conventional key-value-store layouts.

## Why tries are not used as databases

A [trie](https://en.wikipedia.org/wiki/Trie) is a search tree whose path is determined by the symbols of a key. For byte strings, each step consumes one byte. This gives tries several appealing properties for key-value storage.

Lookup depends on key length, $O(k)$, rather than directly on the number of stored records. A search does not compare the requested key with separator keys at every tree level or rely on collision handling. Common prefixes are shared structurally, and ordered traversal follows naturally when child edges are kept in byte order.

Those properties alone do not make a practical database.

The conventional database answer is the [B-tree](https://en.wikipedia.org/wiki/B-tree), or, more commonly, the [B+ tree](https://en.wikipedia.org/wiki/B%2B_tree). B-trees were designed around slow storage and fixed-size pages. Each internal page stores many separator keys and child pointers, producing a high fanout and a small height. A lookup may therefore require only a few page reads, each of which brings in many useful keys. B-trees also support sorted iteration well because neighboring keys are packed into pages. Decades of database engineering have built concurrency, recovery, bulk loading, and cache behavior around this page-oriented model. A simple comparison-based characterization of lookup is $O(k \log n)$.

A naive trie loses this comparison. A byte trie can have up to 256 children per internal node. If every node stores a full 256-entry pointer array, most entries will usually be empty. The database then pays for absence: memory and disk space are reserved for branches that do not exist. A 256-entry array of 8-byte links uses 2 KiB per node, even when only a few children are present.

A linked-list or map-of-children representation saves space, but gives back much of the lookup advantage through extra pointer chasing, variable node layouts, and poorer locality. Long keys can also create chains of single-child nodes, turning one logical key into many stored nodes.

The central problem is therefore space. The direct addressing that makes a trie attractive also creates a sparse node representation. This can be acceptable for in-memory dictionaries, and prefix semantics can justify the cost in specialized domains such as IP routing. For a general persistent key-value store, unused child slots and extra node reads are a substantial tax.

Compressed tries, radix trees, Patricia trees, and adaptive radix trees address this weakness by compressing empty paths, reducing child storage, or selecting node formats according to density.

## Two-Level Bitmap Compression Solves the Space Problem

Instead of using a fixed-size child-pointer array, a two-level bitmap can represent sparse children compactly while preserving direct byte selection. Modern CPUs provide efficient population-count operations for this layout.

![Compression](compress.svg)

Consider a node with children at byte values 5, 70, and 130. A naive representation allocates one child-link slot for every possible byte, retaining 256 offsets although only three are used. The first compression extracts those three offsets into a packed array in byte order: offset 0 belongs to byte 5, offset 1 to byte 70, and offset 2 to byte 130. A 256-bit presence bitmap, held as eight 32-bit words for ranges 0-31 through 224-255, records which byte values have an offset. With 4-byte offsets, this reduces the example from a 1,024-byte offset array to three offsets and 32 bytes of bitmap data, or 44 bytes.

The second compression removes bitmap words that contain no set bits. Bytes 5, 70, and 130 lie in groups 0, 2, and 4, so only those three 32-bit words remain. They are packed into the lower-bitmap array, while an 8-bit upper bitmap records the groups that survived: `0b00010101` has bits 0, 2, and 4 set. The resulting node stores three offsets, three lower bitmap words, and the one-byte upper bitmap: 25 bytes before header and alignment costs.

To find a child for byte `c`, first compute its group as `c >> 5` and test the corresponding bit in the upper bitmap. The population count of the preceding set upper bits identifies that group's packed lower bitmap. Then use `c & 0x1f` to test the bit within the 32-bit word. If that bit is set, population counts over the preceding lower bits determine the child's position in the packed offset array. This preserves a predictable, compact lookup path without reserving storage for absent children.

The trie is also a radix trie: sequences of nodes with a single child are collapsed into a prefix stored at one node.

Conceptually, the variable-sized portion of a `_TrieNode` follows its fixed
header:

```cpp
struct _TrieNode {
    uint16_t _branch_count;       // number of packed branch offsets
    uint8_t _branch_bits_index;   // populated 32-byte groups
    uint8_t _prefix_len;          // radix-prefix length
    uint8_t _branch_bits_pos;     // start of branch_bits[], in uint32_t units
    uint8_t _branch_offsets_pos;  // start of branch_offsets[], in offset_t units
    uint8_t _prefix[];            // compressed key bytes
    uint32_t _branch_bits[];      // one word per populated group
    offset_t _branch_offsets[];   // packed child links in byte order
};
```

The sketch shows the layout rather than literal C++. A concrete implementation uses one variable-sized allocation; `_branch_bits_pos` and `_branch_offsets_pos` locate the packed bitmap and child-link regions after the compressed prefix. Padding preserves the alignment required by the bitmap words and link type.

A separate leaf node stores key and value data:

```cpp
struct LeafNode {
    uint8_t key_length;
    uint8_t key[];
    uint64_t value;
};
```

## Trie Compression Is Only Half the Story: Memory Layout Is Crucial

Compact nodes must also be allocated and reclaimed efficiently. Rather than  using one arbitrary allocation size, the memory manager places each node in a fixed page class. The classes are calculated from the node layout and cover common fanouts: 2, 3, 4, 10, 16, 64, and 256 branches, with additional classes for pages with larger leaves.

![Page-class allocation and transaction-safe recycling](memory-manager.svg)

The memory manager stores one recycling pool for every page class in a compact array. Selecting a class therefore also selects its pool directly. A released page enters the matching FIFO queue together with the transaction that released it. The allocator may reuse the oldest queued page only after all older reader snapshots have advanced beyond that transaction; until then, the page remains in the pool but cannot be repurposed.

An allocation first tries an eligible page from the selected pool. If no page is available, it uses retained leftover space from an earlier area, then bump allocates from the active fixed-size area, and finally obtains a new area. This keeps allocation fast, reuses pages of the correct size, and avoids discarding the useful tail of an exhausted area.

Copy-on-write makes this page layout persistent. An update writes replacement
pages instead of modifying pages reachable from an active snapshot. A commit
publishes the new root only after its replacement pages are complete, allowing
readers to continue from their previous root while old pages wait for safe
recycling.

## How Does It Perform?

Leaves implements the preceding ideas: radix-compressed trie nodes, two-level bitmap-indexed children, page-oriented allocation and recycling, and copy-on-write updates for its persistent storage. The following measurements describe one local run, not a universal ranking. Storage hardware, processor architecture, compiler options, value sizes, and access patterns all affect the result.

### In-Memory Comparison

`bench_memdb_vs_hashtable` compares Leaves' in-memory trie with `std::unordered_map` and `std::map`. The test used one million randomly generated 32-byte binary keys, 100-byte values, and two rounds on Linux 6.8, GCC 13.3, and an Intel Core i7-12700KF. The figures below are microseconds per operation; lower is better.

| Workload | Leaves `_MemoryDB` | `std::unordered_map` | `std::map` |
| --- | ---: | ---: | ---: |
| Sequential fill | 0.162 | 0.442 | 0.202 |
| Random fill | 0.179 | 0.272 | 0.907 |
| Random read | 0.176 | 0.078 | 1.084 |
| Sequential read | 0.068 | 0.065 | 0.089 |
| Overwrite | 0.192 | 0.125 | 1.183 |
| Erase | 0.314 | 0.172 | 1.021 |

![In-memory key-value store benchmark](memorydb-benchmark.svg)

The same run reported 195.0 MiB for `_MemoryDB`, 212.1 MiB for
`std::unordered_map`, and 219.3 MiB for `std::map` after filling the dataset.
Those numbers are allocator-specific process or container measurements, so they
are useful as a local comparison rather than as a portable memory-use guarantee.
The trie performed particularly well on insertion and ordered traversal, while
the _hash table retained its expected advantage for random reads and overwrites.

### Persistent Comparison

For persistent storage, `db_bench_leaves` was compared with
`db_bench_mdb --wmap`. Both programs used one million 16-byte decimal keys,
100-byte values, one million reads, 1,000-operation write batches, and the
workloads `fillseq`, `fillrandom`, `overwrite`, `readrandom`, and `readseq`.
Leaves used `MapStorage` with its write-ahead log disabled; LMDB used writable
memory mapping with asynchronous map updates. The table reports one local run
on the same Linux, compiler, and processor configuration as above.

| Workload | Leaves (microseconds/op) | LMDB `--wmap` (microseconds/op) |
| --- | ---: | ---: |
| Sequential fill | 0.093 | 0.088 |
| Random fill | 0.336 | 0.815 |
| Overwrite | 0.358 | 0.886 |
| Random read | 0.262 | 0.494 |
| Sequential read | 0.042 | 0.016 |

On this workload, Leaves was faster for random writes and random reads, while
LMDB was faster for sequential insertion and sequential traversal. The result
should be treated as a workload-specific observation: neither configuration
includes durable write-ahead logging, and a production evaluation should also
test the required durability mode, dataset size, storage device, and concurrent
access pattern.

For a broader performance discussion, see [Can Persistent Tries Beat LMDB?
Leaves Database Benchmarked](https://hackernoon.com/can-persistent-tries-beat-lmdb-leaves-database-benchmarked).

## Bonus: Replication

Tries have another property that B-trees do not expose as naturally: every
subtree has a semantic name. The path to a node is a key prefix. All keys below
that node share that prefix. If the database stores a _hash for each subtree, then
that _hash becomes a compact statement about all key-value pairs below the prefix.

That is the Merkle-trie idea. A Merkle tree hashes data at the leaves and hashes
children into parent hashes, producing a root _hash that summarizes the whole
structure. If two peers have the same root _hash, they have the same state under
that root. If the root hashes differ, the peers can descend into child hashes and
find the differing subtrees without transferring the entire database. Systems
such as Dynamo-inspired stores, Cassandra, and Riak use this style of
anti-entropy synchronization to avoid sending data that replicas already share.
Ethereum's Merkle Patricia Trie applies a related idea to blockchain state:
paths identify state entries, and hashes make the structure verifiable.

Leaves' replication uses the same structural advantage. `ReplicationDB` extends
the normal database with replication-specific state:

- the main trie stores current key-value data;
- the deletion trie stores deleted keys so removals can be replicated;
- _hash tries cache subtree hashes for both the main trie and the deletion trie.

During a replication session, the sender and receiver do not blindly copy the
whole database. The sender transmits _hash subtries. The receiver compares those
hashes with its local _hash trie. Matching subtrees can be acknowledged and
skipped. Divergent or missing subtrees are expanded and transferred. The process
uses explicit protocol messages such as trie data, subtree acknowledgments,
big-value chunks, fraction completion, and final completion, driven by the
sender and receiver FSMs.

Leaves separates replication into phases. First, it synchronizes the main data
trie. Second, it synchronizes the deletion trie. Third, it streams deferred large
values. When the receiver has the necessary data, it applies the received state
through a staged merge and commits it atomically. For very large sessions, the
receiver can enter fraction mode: it commits a bounded chunk, asks the sender to
restart from the root, and then relies on _hash comparison to skip everything that
has already converged.

This design keeps the mechanism and policy separate. Leaves provides the trie
structure, deterministic message flow, _hash-based pruning, staged apply, and
atomic commit. Applications still decide the consistency model, peer topology,
retry behavior, and conflict-resolution policy.

The performance-oriented node layout and the replication design reinforce one
another. A trie already divides the keyspace into prefix subtrees, so adding
hashes does not require a separate partitioning scheme. The replication
structure follows the storage structure.

## Conclusion

Tries were not kept out of databases because their lookup model was weak. They
were kept out because their obvious representation was too sparse. B-trees won
because they make economical use of pages, keep height low, and behave well on
storage hardware.

Leaves addresses the trie problem at the node-layout level. Prefix compression
removes long single-child chains. The two-level bitmap removes the 256-pointer
array while preserving fast byte selection. Copy-on-write pages make the
structure persistent. Together, those techniques turn a trie from an in-memory
data structure into a viable database layout.

The bonus is replication. A compact persistent trie can also be a Merkle trie:
hashes summarize prefix subtrees, peers compare structure before transferring
data, and replication sends the differences rather than the database. That is
the larger concept behind Leaves: one structure for lookup, persistence,
copy-on-write updates, and synchronization.

## Addendum: Ordering

Tries order keys lexicographically, whereas a B-tree can be parameterized with
an arbitrary comparison function. This distinction matters when an application
requires an order that cannot be represented in the key encoding.

Many practical orderings can, however, be expressed by encoding keys into a
lexicographically sortable binary form. Fixed-width integers can use big-endian
encoding; composite keys can concatenate sortable components. This approach
preserves trie traversal order and avoids the cost of invoking an application
defined comparator during each structural comparison.
