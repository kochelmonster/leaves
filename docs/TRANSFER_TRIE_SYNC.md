# TransferTrie P2P Synchronization

This document describes the TransferTrie-based replication algorithm for efficient P2P synchronization between Leaves database nodes. This approach is optimized for **incremental updates** between nodes that are already mostly in sync.

## Motivation

The traditional Merkle set reconciliation approach (as described in [REPLICATION.md](REPLICATION.md)) requires `O(depth × number_of_diffs)` network round trips. For databases with deep tries and many small changes, this becomes inefficient.

The TransferTrie approach uses a simple **breadth-first push model** with a fixed-size transfer buffer, achieving efficient synchronization without complex trie intermixing.

## Design Goals

1. **Simple Protocol**: Push nodes in breadth-first order, request missing children
2. **Fixed Buffer Size**: Transfer buffer of configurable size (default 1 MB)
3. **Compact Wire Format**: Contiguous buffer with relative offsets
4. **Bidirectional Sync**: Both nodes can send updates to each other
5. **Conflict Resolution**: Pluggable `OverwritePolicy` for handling conflicts
6. **Deletion Support**: Explicit tombstone tracking via deletion database

## Algorithm Overview

### Push-Based Protocol (Node A → Node B)

Node A wants to push its updates to Node B:

```
     Node A (Sender)                           Node B (Receiver)
        │                                         │
        │  ──────── Step 1: Initial Push ───────► │
        │   TransferTrie buffer containing:       │
        │   • Breadth-first nodes from root       │
        │   • Nodes include hash + structure      │
        │   • Small leaves include values (≤4KB)  │
        │   • Buffer filled until max size        │
        │                                         │
        │                        B checks each node:
        │                        • If hash matches local → skip
        │                        • If no local node → needs data
        │                        • If hash differs → needs children
        │                                         │
        │  ◄─────── Step 2: Request Children ──── │
        │   List of node paths that need          │
        │   their children expanded               │
        │                                         │
        │  ──────── Step 3: Send Subtries ──────► │
        │   TransferTrie buffer with requested    │
        │   subtries (can contain multiple)       │
        │                                         │
        │         (Repeat steps 2-3 until done)   │
        │                                         │
        │  ◄─────── Step 4: Complete ──────────── │
        │   B opens transaction, merges all       │
        │   received nodes from temp DB           │
        │                                         │
```

### Hash Matching Rules

When Node B receives a node from Node A:

| Local State | Remote Hash | Action |
|-------------|-------------|--------|
| Node exists | Hash matches | Skip (subtree identical) |
| Node exists | Hash differs | Request children |
| No node | N/A | Accept and request children |
| Leaf node | N/A | Apply value (using OverwritePolicy) |

### Transfer Buffer Size

**Recommended: 1 MB**

Rationale:
- Large enough to transfer many nodes efficiently
- Small enough to fit in memory and typical network buffers
- Matches typical TCP window sizes for good throughput
- Can be tuned based on network conditions

### Leaf Value Handling

All leaf nodes fit within 4 KB (see `PAGE_SIZES` in storage traits). Big values (>4KB) are stored as references to chunk storage (see `_bigmemory.hpp`), so the leaf itself only contains:
- Key
- Hash (of full key + value)
- Reference to chunk storage (offset + size)

The TransferTrie transfers the leaf node as-is. The chunk storage data is transferred separately after structure sync.

## TransferTrie Structure

### Node Types

```cpp
enum class TransferNodeType : uint8_t {
    TRIE_NODE   = 0x01,  // Trie node with path, hash, and child references
    LEAF_NODE   = 0x02   // Leaf node (always ≤4KB, may contain big value reference)
};

enum class DbType : uint8_t {
    DB_MAIN     = 0x00,  // Main data database
    DB_DELETION = 0x01   // Deletion records database
};
```

Note: All nodes fit within 4 KB (`PAGE_SIZES` constraint). Big values are stored as references to chunk storage, not inline data.

### Wire Format

A sync buffer can contain **multiple TransferTrie structures**, each with its own header. This allows responding to multiple REQUEST_CHILDREN paths in a single network message.

Each TransferTrie is serialized as:

```
┌──────────────────────────────────────────────────────────────┐
│ Header                                                       │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ magic: uint32_t = 0x4C565354 ("LVST")                    │ │
│ │ version: uint16_t = 1                                    │ │
│ │ flags: uint16_t                                          │ │
│ │ db_type: uint8_t (0x00 = main DB, 0x01 = deletion DB)    │ │
│ │ node_count: uint32_t                                     │ │
│ │ total_size: uint64_t                                     │ │
│ │ session_id: uint64_t (random, generated at sync start)   │ │
│ │ snapshot_id: uint64_t (transaction ID for consistency)   │ │
│ │ subtrie_path_len: uint16_t                               │ │
│ │ subtrie_path: uint8_t[subtrie_path_len] (path to root    │ │
│ │               of this subtrie, empty for full DB sync)   │ │
│ └──────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│ Node Data (variable length, breadth-first order)             │
│ Nodes are stored in the same format as in the database.      │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ Node 0: TRIE_NODE                                        │ │
│ │   type: uint8_t = 0x01                                   │ │
│ │   node_size: uint16_t      (size of trie node data)      │ │
│ │   node_data: uint8_t[node_size] (raw trie node from DB)  │ │
│ │   (contains compressed_path, hash, child offsets, etc.)  │ │
│ ├──────────────────────────────────────────────────────────┤ │
│ │ Node 1: LEAF_NODE                                        │ │
│ │   type: uint8_t = 0x02                                   │ │
│ │   node_size: uint16_t      (size of leaf node data)      │ │
│ │   node_data: uint8_t[node_size] (raw leaf node, ≤4KB)    │ │
│ └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

Note: SUBTRIE_REQ is not used in TransferTrie buffers. Instead, the receiver sends a REQUEST_CHILDREN message.

### Byte Order

All multi-byte integers use **little-endian** encoding. This differs from network byte order (big-endian) but is chosen because:
- Most modern CPUs are little-endian (x86, ARM in LE mode)
- No byte-swapping overhead on common platforms
- Matches the database's native storage format
- Uses `boost::endian::little_uint*_t` types for explicit encoding

### Node Format

Nodes are stored in **breadth-first order** and use the **same binary format as in the database**. This allows:
- Zero-copy parsing - nodes can be used directly
- No serialization/deserialization overhead
- Direct memcpy into temp DB storage

The `subtrie_path` in the header specifies the path from the database root to the first node in this buffer. For initial sync, this is empty. For child requests, it contains the path to the requested subtrie.

### Size Limits

| Parameter | Default | Rationale |
|-----------|---------|-----------|
| Transfer buffer size | 1 MB | Good balance of throughput and memory |
| Max node size | 4 KB | PAGE_SIZES constraint in storage traits |
| Big value chunk | 64 KB | Streaming chunk size for chunk storage sync |

## Big Value Handling

Big values (>4KB) are stored in separate chunk storage (see `_bigmemory.hpp`). The leaf node itself only contains a reference (`BigValue` struct with offset, size, and security_token). During sync:

1. **Structure sync**: Leaf nodes (including big value references) are transferred normally
2. **Chunk sync**: After structure sync completes, receiver requests missing chunks
3. **Chunk storage**: Received chunks are written to temp DB's own BigMemory storage
4. **Streaming**: Large chunks are sent in 64 KB pieces
5. **Merge**: At sync completion, temp BigMemory is merged into main BigMemory

### Sync Completion

The receiver sends SYNC_COMPLETE when:
- Flow control queue is empty (all subtries received), AND
- All chunk requests are satisfied (chunks written to BigMemory)

```cpp
struct ChunkSyncRequest {
    std::vector<ChunkRef> chunks;  // Chunks needed by receiver
};

struct ChunkRef {
    uint64_t offset;        // Offset in sender's chunk storage
    uint64_t security_token; // Random token from BigValue struct (anti-tampering)
};

struct ChunkSyncResponse {
    struct Entry {
        uint64_t offset;
        uint64_t total_size;
        uint64_t data_offset;  // Offset within chunk (for streaming)
        Slice data;            // Up to 64KB
        bool is_final;
    };
    std::vector<Entry> entries;
};
```

### Chunk Security

To prevent malicious chunk offset requests:
1. When storing a big value, sender generates a random 64-bit `security_token`
2. The `security_token` is stored in the `BigValue` struct alongside offset/size
3. Receiver includes both `offset` and `security_token` in CHUNK_REQ
4. Sender verifies `security_token` matches before sending chunk data
5. Mismatched tokens result in SYNC_ERROR

## Sync Initiation

The **sender initiates sync** after a committed transaction:
1. Transaction commits successfully
2. Short delay (configurable, e.g., 100ms) to batch rapid commits
3. Sender opens read transaction (snapshot) for sync
4. Sender pushes initial TransferTrie to receiver(s)

## Flow Control

The receiver uses a **queue-based flow control** mechanism:

```cpp
class ReceiverFlowControl {
    std::queue<SubtrieRequest> pending_requests;
    
public:
    void process_incoming(const TransferTrie& trie) {
        // Add new subtrie requests to queue
        for (auto& path : extract_needed_children(trie)) {
            pending_requests.push({trie.db_type(), path});
        }
    }
    
    RequestChildren build_next_request() {
        RequestChildren request;
        
        while (!pending_requests.empty() && !request.is_full()) {
            auto req = pending_requests.front();
            pending_requests.pop();
            
            // Check if subtrie already received in temp DB
            if (temp_db.has_subtrie(req.path)) {
                // Already have it - skip
                continue;
            }
            
            // Not yet received - request it and re-queue for later check
            request.add_path(req.db_type, req.path);
            pending_requests.push(req);  // Re-queue to verify later
        }
        
        return request;
    }
    
    bool is_complete() {
        // Complete when queue is empty (all subtries verified in temp DB)
        return pending_requests.empty();
    }
};
```

This approach:
- Prevents overwhelming the sender with requests
- Handles out-of-order responses gracefully
- Naturally deduplicates requests
- Detects completion when queue drains

## Deletion Handling

### The Problem

Merkle tries don't inherently track deletions. If Node A deletes key "foo", its trie simply lacks that key. Node B has no way to distinguish "A never had foo" from "A deleted foo".

### Solution: Deletion Database

Each node maintains a separate **deletion database** (also a trie) that records:
- Deleted key
- Deletion timestamp (or sequence number)
- Optional: Previous value hash (for conflict detection)

```cpp
struct DeletionRecord {
    Slice key;
    uint64_t timestamp;  // When deletion occurred
    Hash prev_hash;      // Hash of deleted value (optional)
};
```

### Sync Order: Both Databases Together

The main database and deletion database are **synced together in one synchronization round**:

```
Sync Round (A → B):
1. Push main DB TransferTrie
2. Push deletion DB TransferTrie (same protocol)
3. B builds temp tries for both
4. B merges both using OverwritePolicy
```

The merge order (main vs deletion first) does not matter - the `OverwritePolicy` decides conflicts based on timestamps or other criteria, not merge order.

This ensures that:
- Deletions and updates are applied atomically
- Conflicts between delete and update are resolved by policy
- No resurrection of deleted keys from stale data

### Deletion Record Expiry

Deletion records can be pruned after a configurable retention period (e.g., 7 days). Nodes that haven't synced within that period must perform a full sync.

## Conflict Resolution

### OverwritePolicy Interface

```cpp
template <typename Hash>
struct OverwritePolicy {
    // Return true to accept remote value, false to keep local
    virtual bool should_overwrite(
        const Slice& key,
        const Slice& local_value,
        const Hash& local_hash,
        const Slice& remote_value,
        const Hash& remote_hash
    ) const = 0;
};
```

### Built-in Policies

```cpp
// Always accept remote value (simple primary-replica model)
struct AlwaysAcceptRemote : OverwritePolicy<Hash> {
    bool should_overwrite(...) const override { return true; }
};

// Accept if remote has newer timestamp (requires timestamp in value)
struct LastWriteWins : OverwritePolicy<Hash> {
    bool should_overwrite(...) const override {
        return extract_timestamp(remote_value) > 
               extract_timestamp(local_value);
    }
};

// Never overwrite existing values
struct NeverOverwrite : OverwritePolicy<Hash> {
    bool should_overwrite(...) const override { return false; }
};
```

## Edge Cases

### 1. Empty Receiver (Initial Sync)

When Node B is empty, every node from A is new:
- All trie nodes trigger child requests
- Transfer proceeds breadth-first until complete
- May require many rounds for large databases
- Consider compression for bandwidth efficiency

### 2. Mostly Identical Databases

When databases are nearly identical:
- Root hash comparison quickly identifies matching subtrees
- Only differing branches are transferred
- Most efficient case for incremental sync

### 3. Buffer Overflow Mid-Subtrie

If the 1 MB buffer fills before a subtrie is complete:
- Sender returns what fits in the buffer
- Remaining children have **offset = 0** in parent's child array (indicates "not yet sent")
- Receiver detects 0 offsets when processing parent nodes
- Receiver adds those paths to REQUEST_CHILDREN queue
- Breadth-first order ensures parent nodes arrive before children

The receiver identifies incomplete subtries by checking child offsets in received trie nodes. A 0 offset means that child was not included in this buffer and needs to be requested.

### 4. Concurrent Writes During Sync

The sender operates on a read transaction (snapshot) captured at sync start:
1. Sender opens read transaction, captures `snapshot_id`
2. All reads during sync use that snapshot (isolated from concurrent writes)
3. Changes made after snapshot start are not visible to this sync
4. After sync completes, sender can push a new sync round to capture new changes

The `TransferTrie` header includes:
- `session_id`: Unique ID for this sync session (generated at sync start, constant across all rounds)
- `snapshot_id`: Transaction ID of the sender's read snapshot

The receiver uses `session_id` to:
- Correlate buffers belonging to the same sync session
- Distinguish simultaneous syncs from different sender nodes
- Associate temp DB state with the correct session

### 5. Simultaneous Bidirectional Sync

The protocol supports simultaneous bidirectional sync (A→B and B→A at the same time):
- Each direction builds its own temp DB
- Merges are applied with OverwritePolicy
- Conflicts are resolved deterministically by policy
- No deadlocks since each side works on its own temp DB

### 6. Error Recovery

On network error or protocol failure during sync:

**Receiver (Node B) behavior:**
1. Start timeout timer (configurable, e.g., 30 seconds)
2. If network recovers within timeout: resend last REQUEST_CHILDREN
3. Sender receives duplicate request and responds normally (idempotent)
4. Session continues from where it left off
5. If timeout expires: discard temp DB for that `session_id`

**Sender (Node A) behavior:**
1. Keep read transaction and BFS state alive during timeout window
2. If receiver resends request: respond with same data (idempotent)
3. If timeout expires: close read transaction, clean up session state

```cpp
// Configurable timeout (default 30 seconds)
static constexpr auto SESSION_TIMEOUT = std::chrono::seconds(30);
```

The `session_id` (random 64-bit, generated via `std::random_device`) enables:
- Correlating retry requests to the correct session
- Rejecting stale requests from expired sessions
- Maintaining multiple concurrent sync sessions from different nodes

This approach handles transient network issues gracefully while keeping the protocol simple and stateless across session boundaries.

## Integration with Merger

The temp DB is a full trie structure, allowing use of the existing `Merger` infrastructure:

```cpp
template <typename OverwritePolicy>
void merge_temp_to_main(TempDB& temp, MainDB& main, OverwritePolicy& policy) {
    // Use Merger to iterate both tries in sorted order
    Merger merger(temp.cursor(), main.cursor());
    
    auto txn = main.start_transaction();
    
    while (merger.next()) {
        if (merger.has_remote_only()) {
            // New key from remote - insert
            main.cursor().insert(merger.key(), merger.remote_value());
        } else if (merger.has_both()) {
            // Key exists in both - check policy
            if (policy.should_overwrite(merger.key(),
                                        merger.local_value(),
                                        merger.local_hash(),
                                        merger.remote_value(),
                                        merger.remote_hash())) {
                main.cursor().update(merger.key(), merger.remote_value());
            }
        }
        // Local-only keys are kept as-is
    }
    
    main.commit();
    temp.clear();
}
```

## Replication Policies

Replication policies define the high-level sync behavior. They compose OverwritePolicy with sync direction and timing.

### Planned Policies

```cpp
// Master-Slave: One-way sync from master to replicas
struct MasterSlavePolicy {
    // Master pushes to all slaves
    // Slaves never push to master
    // OverwritePolicy: AlwaysAcceptRemote on slaves
};

// P2P: Bidirectional sync between peers
struct PeerToPeerPolicy {
    // Both nodes push to each other
    // Can happen simultaneously
    // OverwritePolicy: Application-defined (e.g., LastWriteWins, VersionVector)
};

// Multi-Master: Multiple masters with conflict resolution
struct MultiMasterPolicy {
    // All nodes can write
    // Sync in both directions
    // OverwritePolicy: Must handle conflicts (version vectors, CRDTs, etc.)
};
```

### Policy Selection

| Use Case | Recommended Policy |
|----------|-------------------|
| Read replicas | Master-Slave |
| Offline-first apps | P2P with LastWriteWins |
| Distributed databases | Multi-Master with VersionVector |
| Event sourcing | Master-Slave (log is master) |

## Performance Analysis

### Network Round Trips

| Scenario | Round Trips |
|----------|-------------|
| Identical databases | 1 (root hash matches) |
| Few scattered changes | 2-3 (initial + child requests) |
| Many changes, shallow | 2-3 |
| Many changes, deep | O(depth) in worst case |
| Initial sync (empty receiver) | O(depth) |

### Bandwidth Efficiency

| Component | Size |
|-----------|------|
| Trie node overhead | ~35 bytes (type + path + hash + children) |
| Leaf node | Raw node data (≤4KB) + path overhead |

### Buffer Size Tradeoffs

| Buffer Size | Pros | Cons |
|-------------|------|------|
| 256 KB | Low memory, fast start | More round trips |
| 1 MB | Good balance | Default choice |
| 4 MB | Fewer round trips | Higher memory, latency |
| 16 MB | Minimal round trips | High memory usage |

## Implementation Roadmap

### Phase 1: Core Data Structures
- [ ] `TransferTrie` buffer format and header
- [ ] Node type serializers (TRIE_NODE, LEAF_NODE)
- [ ] Breadth-first buffer builder

### Phase 2: Sender Implementation
- [ ] Breadth-first trie traversal with buffer limit
- [ ] Path tracking for node identification
- [ ] Subtrie request handling

### Phase 3: Receiver Implementation
- [ ] TransferTrie parser
- [ ] Hash comparison with local DB (by path)
- [ ] Temp DB construction (full trie structure)
- [ ] Child request generation

### Phase 4: Merge and Apply
- [ ] Merger integration for temp→main merge
- [ ] OverwritePolicy integration
- [ ] Chunk storage sync for big values

### Phase 5: Deletion Support
- [ ] Deletion database (same trie format)
- [ ] Sync both DBs in one round
- [ ] Expiry and pruning

### Phase 6: Replication Policies
- [ ] MasterSlavePolicy
- [ ] PeerToPeerPolicy
- [ ] MultiMasterPolicy with version vectors

### Phase 7: Optimizations
- [ ] Compression (LZ4 for speed, Zstd for ratio)
- [ ] Parallel subtrie transfers
- [ ] Connection pooling

## Wire Protocol Messages

```
Message Types:
  0x40 TRANSFER_TRIE    { TransferTrie[] }   // One or more TransferTries (main or deletion DB)
  0x41 REQUEST_CHILDREN { header, paths[] }  // Request subtries
  
  0x42 CHUNK_REQ        { session_id, chunks[] }  // Request big value chunks
  0x43 CHUNK_DATA       { session_id, entries[] }
  
  0x46 SYNC_COMPLETE    { session_id, snapshot_id, stats }  // Sent by RECEIVER
  0x47 SYNC_ERROR       { session_id, error_code, message }
```

### REQUEST_CHILDREN Format

```
┌──────────────────────────────────────────────────────────────┐
│ Header                                                       │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ magic: uint32_t = 0x4C565352 ("LVSR")                    │ │
│ │ session_id: uint64_t                                     │ │
│ │ db_type: uint8_t (0x00 = main DB, 0x01 = deletion DB)    │ │
│ │ path_count: uint32_t                                     │ │
│ └──────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│ Path Data                                                    │
│ ┌──────────────────────────────────────────────────────────┐ │
│ │ path_len: uint16_t                                       │ │
│ │ path: uint8_t[path_len]                                  │ │
│ └──────────────────────────────────────────────────────────┘ │
│ ... (repeated path_count times)                              │
└──────────────────────────────────────────────────────────────┘
```

### CHUNK_REQ Format

Sent by receiver after structure sync to request missing big value chunks:

```
┌──────────────────────────────────────────────────────────────┐
│ session_id: uint64_t                                         │
│ chunk_count: uint32_t                                        │
│ chunks: ChunkRef[chunk_count]                                │
│   - offset: uint64_t (offset in sender's chunk storage)      │
│   - security_token: uint64_t (from BigValue struct)          │
└──────────────────────────────────────────────────────────────┘
```

The receiver extracts `offset` and `security_token` from received `BigValue` structs. The sender verifies the `security_token` matches before sending chunk data, preventing malicious memory access attempts.

## Security Considerations

**Handled by this layer:**
1. **Validate TransferTrie structure**: Malformed buffers could cause crashes
2. **Verify hashes before applying**: Don't trust values without verification
3. **Chunk security tokens**: Verify `security_token` before sending chunk data
4. **Rate limit sync requests**: Prevent DoS via expensive TransferTrie generation

**Handled by other layers:**
- **Authentication**: Node identity verification (TLS, certificates, etc.)
- **Authorization**: Permission to sync (access control lists)
- **Compression**: Data compression (LZ4, Zstd) applied at transport layer
- **Encryption**: Data encryption (TLS)

## Example Usage

```cpp
#include <leaves/sync.hpp>

// BFS queue node tracks both the source offset and destination link
struct QueueNode {
    offset_t node_offset;  // Offset in source DB
    offset_t* link;        // Pointer to parent's child array slot in TransferTrie
};

// Build a TransferTrie for a subtrie
class SyncSender {
    LocalDB& db;
    
public:
    TransferTrie build_transfer_trie(const Slice& subtrie_path, 
                                     size_t max_size = 1024 * 1024) {
        TransferTrie buffer(max_size);
        buffer.set_subtrie_path(subtrie_path);
        buffer.set_db_type(DB_MAIN);  // or DB_DELETION
        
        std::queue<QueueNode> bfs_queue;
        
        // Find the root of the requested subtrie
        offset_t first = db.find_offset(subtrie_path);
        if (!first.valid()) return buffer;  // Subtrie not found
        
        // Add root node
        if (first.is_leaf()) {
            auto leaf = db.resolve_leaf(first);
            buffer.add_node(LEAF_NODE, leaf, nullptr);
        } else {
            auto trie = db.resolve_trie(first);
            auto dtrie = buffer.add_node(TRIE_NODE, trie, nullptr);
            
            // Queue children for BFS, with links to parent's child slots
            for (uint16_t i = 0; i < trie.count(); ++i) {
                bfs_queue.push({trie.array()[i], dtrie.array() + i});
            }
        }
        
        // BFS traversal
        while (!bfs_queue.empty() && !buffer.is_full()) {
            auto [node_offset, link] = bfs_queue.front();
            bfs_queue.pop();
            
            if (node_offset.is_leaf()) {
                auto leaf = db.resolve_leaf(node_offset);
                buffer.add_node(LEAF_NODE, leaf, link);
            } else {
                auto trie = db.resolve_trie(node_offset);
                auto dtrie = buffer.add_node(TRIE_NODE, trie, link);
                
                for (uint16_t i = 0; i < trie.count(); ++i) {
                    bfs_queue.push({trie.array()[i], dtrie.array() + i});
                }
            }
        }
        
        // Remaining queued nodes will be requested by receiver via REQUEST_CHILDREN
        return buffer;
    }
};

// Receive and process sync data  
class SyncReceiver {
    LocalDB& db;
    std::unordered_map<uint64_t, TempDB> sessions;  // session_id -> temp DB
    
public:
    // Process received TransferTrie buffer (may contain multiple TransferTries)
    RequestChildren process(const Buffer& received) {
        RequestChildren request;
        
        for (auto& trie : received.transfer_tries()) {
            auto session_id = trie.session_id();
            auto& temp = sessions[session_id];  // Get or create temp DB
            
            // Insert nodes into temp DB using same structure as main DB
            // (see _Inserter for big key handling with intermediate trie nodes)
            for (auto& node : trie) {
                auto full_path = trie.subtrie_path() + node.compressed_path();
                
                if (node.type == TRIE_NODE) {
                    auto local_hash = db.get_hash_at_path(full_path);
                    if (local_hash && *local_hash == node.hash()) {
                        // Subtree matches - skip
                        continue;
                    }
                    // Hash mismatch or missing - store and request children
                    request.add_path(trie.db_type(), full_path);
                }
                
                // Store in temp DB (handles long paths with intermediate nodes)
                temp.insert_node(full_path, node.data(), node.size());
            }
        }
        
        return request;
    }
    
    // Check if sync is complete (no more children needed)
    bool is_complete(uint64_t session_id) {
        return sessions[session_id].is_complete();
    }
    
    // Finalize: merge temp trie into main DB
    void finalize(uint64_t session_id, OverwritePolicy& policy) {
        auto& temp = sessions[session_id];
        Merger::merge(temp, db, policy);
        sessions.erase(session_id);
    }
};
```

## Temp DB Structure

The receiver's temp DB uses the **same trie structure as the main DB**:
- Nodes with paths longer than `compressed_path` capacity use intermediate trie nodes
- This matches the `_Inserter` big key handling in the main database
- Enables direct use of `Merger` for final merge
- No special path reconstruction needed

### Temp BigMemory Storage

The temp DB has its **own BigMemory storage** for received chunks:
- Chunks are written to temp BigMemory during sync
- At finalization, temp BigMemory is merged into main BigMemory
- BigValue references in temp trie are updated to point to new main offsets

**Abort handling**: On sync abort or timeout, the temp DB (including its BigMemory) is simply discarded. No cleanup of main storage needed - all temp data is isolated.

```cpp
class TempDB {
    Trie trie;           // Temp trie structure
    BigMemory big_mem;   // Temp chunk storage
    
public:
    void store_chunk(const ChunkRef& ref, Slice data) {
        // Write to temp BigMemory, track mapping from sender offset
        auto new_offset = big_mem.write(data);
        offset_map[ref.offset] = new_offset;
    }
    
    void finalize_to(MainDB& main) {
        // Merge BigMemory first, updating offsets
        auto offset_remap = main.big_mem.merge_from(big_mem);
        
        // Update BigValue references in temp trie
        update_bigvalue_offsets(trie, offset_remap);
        
        // Then merge trie
        Merger::merge(trie, main.trie, policy);
    }
    
    void discard() {
        // Simply free temp storage - no main DB changes
        trie.clear();
        big_mem.free();
    }
};
```

## References

- [REPLICATION.md](REPLICATION.md) - Original replication design based on Ethereum's approach

