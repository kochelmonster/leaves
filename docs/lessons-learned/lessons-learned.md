# Lessons Learned

This document summarizes the engineering lessons that shaped the current Leaves architecture.

## Data Locality

Several experiments were conducted to improve data locality for trie nodes.

Simple Hint
: Use an allocation hint for the memory pool to place a block near its parent node.
  Result: No measurable performance improvement was observed.

Node Cluster
: Cluster child nodes with their parent in a single memory block during insertion.
  Result: Performance declined significantly because the clustering step—especially the additional memcpy work—was more expensive than the locality benefit it provided.

Cluster Leaves
: Cluster neighboring leaves in a single memory block.
  Result: Performance again decreased. The extra clustering work, particularly during page splits, outweighed the benefit of improved locality.

## Multithreaded Hash Updater

This approach was removed because the performance gain did not justify the added implementation complexity. Network transfer remains the primary bottleneck.

## Big Values in MMAP

- When a value exceeds a certain size threshold, it is faster to write it directly to the mmap file than to copy it into memory first.
- The threshold depends on the system and must be determined through benchmarking.

## WAL Semantics as a Second ACID Option

Because synchronously flushing a memory-mapped file can severely reduce performance, a WAL mechanism was introduced to provide fast ACID transactions without the same penalty. In the db_bench_leaves benchmark, it led to a substantial increase in write throughput. Interestingly, that speed advantage did not appear in the workload_kv_rmw scenario of the YCSB-CPP benchmark. The reason remains unclear, and a deeper investigation did not reveal an obvious explanation.

## 256-Array TrieNode

The original idea was to use a bitmap to compress the pointer map of a trie node. The first obvious choice was a 64-bit value. However, the additional work required to transform a key string into its 64-bit representation hurt performance. Larger fanouts performed better. The final design used the double-bitmap compression approach described in the architecture document.

### Branch Key Always Compressed

The branch key is always stored in compressed form rather than being recomputed from the TrieNode bitmaps. This approach is both simpler to implement and faster: reconstructing the key string requires only concatenating the compressed parts of the nodes from the root to the leaf.

## Development

### Printing Trie Structures Eases Development

Working with trie structures is inherently complex. Printing them in a human-readable form proved invaluable for debugging and development. Visualizing trie structures with tests/graph.py was particularly helpful for understanding their structure and identifying bugs in the implementation.

**Example:**

![Trie structure visualization showing leaf split](insert_leaf_split_2_abc_e_ghi.svg)

This visualization shows how a trie changes when keys such as "abc", "e", and "ghi" are inserted and split across nodes, making it immediately clear whether the tree structure is correct and whether there are issues with node boundaries or compression logic.

