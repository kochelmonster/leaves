# Leaves Replication Guide

This document describes how to enable replication for Leaves databases and the workflow for synchronizing data between nodes. The design is informed by Ethereum's go-ethereum merkle trie synchronization, which has been battle-tested at scale.

## Overview

Leaves supports Merkle trie replication, enabling efficient synchronization between database replicas. The approach combines two complementary strategies learned from Ethereum:

1. **Range Sync** (inspired by Ethereum's Snap Sync) - Bulk transfer of contiguous key ranges with edge proofs
2. **Trie Sync** (inspired by Ethereum's Trie Healing) - Node-by-node synchronization for filling gaps and verification

This hybrid approach achieves:
- **Fast initial sync**: Bulk data transfer without downloading intermediate trie nodes
- **Cryptographic verification**: Merkle proofs ensure data integrity
- **Incremental updates**: Only changed subtrees are synchronized
- **Resumable sync**: Can continue from where it left off after interruption

## Lessons from Ethereum's go-ethereum

### What Ethereum Does Right

1. **Two-Phase Sync**: Snap sync for bulk data, then trie healing for completeness
2. **Range Proofs**: Prove that a range of keys is complete (no missing entries)
3. **Batched Requests**: Request multiple nodes/ranges in parallel
4. **Priority Scheduling**: Prioritize ranges with the most data
5. **Proof Verification Before Apply**: Never trust, always verify

### Key Components from go-ethereum

| go-ethereum Component | Purpose | Leaves Equivalent |
|-----------------------|---------|-------------------|
| `trie/sync.go` | Node-by-node trie sync scheduler | `_TrieSync` |
| `trie/proof.go` | Merkle proof generation/verification | `_MerkleProof` |
| `eth/protocols/snap/sync.go` | Range-based bulk sync | `_RangeSync` |
| `core/state/sync.go` | State synchronization orchestrator | `_SyncScheduler` |

## Enabling Replication

### 1. Use Replicating Storage Classes

```cpp
#include <leaves/replicating_mmap.hpp>   // For memory-mapped storage
#include <leaves/replicating_fstore.hpp> // For file-based storage

// Memory-mapped replicating storage
auto storage = leaves::ReplicatingMapStorage::create("mydb.lvs");
auto db = (*storage)["mydb"];

// File-based replicating storage  
auto storage = leaves::ReplicatingFileStorage::create("mydb.lvs");
auto db = (*storage)["mydb"];
```

### 2. Key Differences from Non-Replicating Storage

| Feature | MapStorage / FileStorage | ReplicatingMapStorage / ReplicatingFileStorage |
|---------|--------------------------|------------------------------------------------|
| Node hash size | 0 bytes (disabled) | 32 bytes (BLAKE3) |
| Hash computation | None | Automatic on `prepare_commit()` |
| Storage overhead | Minimal | +32 bytes per node |
| Sync capability | None | Full Merkle proof support |

## How Merkle Hashing Works

### Automatic Hashing on Commit

When `prepare_commit()` is called, the replication hasher:

1. Traverses the trie depth-first from the root
2. For each node where `node->txn_id == current_transaction_id`:
   - **Leaf nodes**: `hash = BLAKE3(key || value)`
   - **Trie nodes**: `hash = BLAKE3(compressed_path || child_hash_1 || child_hash_2 || ...)`
3. Stores the hash in the node's `hash` field (32 bytes)

This mirrors Ethereum's approach where state root is computed after each block.

### Hash Structure

```
Root Trie Node
├── hash = BLAKE3(path || child_hashes...)
├── child[0] → Trie Node
│   ├── hash = BLAKE3(path || child_hashes...)
│   └── child[0] → Leaf Node
│       └── hash = BLAKE3(key || value)
└── child[1] → Leaf Node
    └── hash = BLAKE3(key || value)
```

## Replication Architecture

### Components to Implement

Based on go-ethereum's architecture, Leaves replication requires:

```
┌─────────────────────────────────────────────────────────────┐
│                     _SyncScheduler                          │
│  (Orchestrates the entire sync process)                     │
├─────────────────────────────────────────────────────────────┤
│                          │                                  │
│    ┌─────────────────────┴─────────────────────┐            │
│    │                                           │            │
│    ▼                                           ▼            │
│ ┌──────────────┐                    ┌──────────────────┐    │
│ │  _RangeSync  │                    │    _TrieSync     │    │
│ │              │                    │                  │    │
│ │ • Bulk data  │                    │ • Node-by-node   │    │
│ │ • Edge proofs│                    │ • Gap filling    │    │
│ │ • Fast       │                    │ • Verification   │    │
│ └──────────────┘                    └──────────────────┘    │
│         │                                    │              │
│         └────────────────┬───────────────────┘              │
│                          ▼                                  │
│                  ┌───────────────┐                          │
│                  │ _MerkleProof  │                          │
│                  │               │                          │
│                  │ • Generate    │                          │
│                  │ • Verify      │                          │
│                  │ • Range proof │                          │
│                  └───────────────┘                          │
└─────────────────────────────────────────────────────────────┘
```

### 1. _MerkleProof (from `trie/proof.go`)

Generates and verifies Merkle proofs:

```cpp
template <typename Cursor>
struct _MerkleProof {
    using hash_t = typename Cursor::Traits::hash_t;
    
    // Generate proof for a single key
    // Returns: vector of node hashes from root to leaf
    static std::vector<hash_t> prove(Cursor& cursor, const Slice& key);
    
    // Verify a proof against expected root hash
    static bool verify(const hash_t& root_hash, 
                       const Slice& key,
                       const Slice& value,
                       const std::vector<hash_t>& proof);
    
    // Generate range proof (edge proofs for a key range)
    // This proves that [start_key, end_key] is complete
    struct RangeProof {
        std::vector<hash_t> left_edge;   // Proof for start_key
        std::vector<hash_t> right_edge;  // Proof for end_key  
        std::vector<std::pair<Slice, Slice>> entries; // All k/v in range
    };
    
    static RangeProof prove_range(Cursor& cursor, 
                                   const Slice& start_key,
                                   const Slice& end_key,
                                   size_t max_entries);
    
    // Verify range proof - ensures no entries are missing
    static bool verify_range(const hash_t& root_hash,
                             const Slice& start_key,
                             const Slice& end_key,
                             const RangeProof& proof);
};
```

### 2. _RangeSync (from `eth/protocols/snap/sync.go`)

Bulk synchronization of key ranges:

```cpp
template <typename DB>
struct _RangeSync {
    struct RangeRequest {
        Slice start_key;
        Slice end_key;
        hash_t expected_root;
    };
    
    struct RangeResponse {
        std::vector<std::pair<Slice, Slice>> entries;
        std::vector<hash_t> proof;
        bool more;  // More data available in this range
    };
    
    // Request side: generate range request
    RangeRequest create_request(const Slice& start, const Slice& end);
    
    // Response side: fulfill range request  
    RangeResponse fulfill_request(const RangeRequest& req, size_t max_bytes);
    
    // Apply received range (with verification)
    bool apply_range(const RangeResponse& response, 
                     const hash_t& expected_root);
};
```

### 3. _TrieSync (from `trie/sync.go`)

Node-by-node synchronization and healing:

```cpp
template <typename DB>
struct _TrieSync {
    // Track what we need
    struct SyncState {
        std::set<hash_t> missing_nodes;    // Nodes we need
        std::set<hash_t> pending_requests; // Nodes we've requested
        std::map<hash_t, offset_t> known_nodes; // Nodes we have
    };
    
    // Initialize sync from a root hash
    void init(const hash_t& target_root);
    
    // Get next batch of node hashes to request
    std::vector<hash_t> missing(size_t max_count);
    
    // Process received nodes
    // Returns: number of nodes successfully processed
    size_t process(const std::vector<std::pair<hash_t, NodeData>>& nodes);
    
    // Check if sync is complete
    bool complete() const;
    
    // Get current progress (0.0 to 1.0)
    float progress() const;
};
```

### 4. _SyncScheduler (from `core/state/sync.go`)

Orchestrates the sync process:

```cpp
template <typename DB>
struct _SyncScheduler {
    enum class Phase {
        IDLE,
        RANGE_SYNC,      // Bulk data transfer
        TRIE_HEALING,    // Fill gaps with node-by-node sync
        COMPLETE
    };
    
    Phase phase;
    _RangeSync<DB> range_sync;
    _TrieSync<DB> trie_sync;
    
    // Start sync to target root hash
    void start(const hash_t& target_root);
    
    // Get next action to perform
    struct Action {
        enum Type { REQUEST_RANGE, REQUEST_NODES, APPLY_DATA, DONE };
        Type type;
        std::variant<RangeRequest, std::vector<hash_t>> request;
    };
    Action next_action();
    
    // Feed response data
    void on_range_response(const RangeResponse& response);
    void on_nodes_response(const std::vector<NodeData>& nodes);
    
    // Handle errors/timeouts
    void on_timeout(const hash_t& request_id);
    void on_error(const std::string& error);
};
```

## Replication Workflow

### Phase 1: Range Sync (Bulk Data Transfer)

```
Primary A                              Replica B
    │                                      │
    │  1. "Give me root hash"              │
    │◄─────────────────────────────────────│
    │                                      │
    │  2. root_hash = 0xABC...             │
    │─────────────────────────────────────►│
    │                                      │
    │  3. "Give me keys [0x00, 0xFF]"      │
    │◄─────────────────────────────────────│
    │                                      │
    │  4. entries[] + edge_proofs          │
    │─────────────────────────────────────►│
    │                                      │
    │         B verifies proof,            │
    │         applies entries              │
    │                                      │
    │  5. "Give me keys [0xFF, 0xFFFF]"    │
    │◄─────────────────────────────────────│
    │         ... continues ...            │
```

### Phase 2: Trie Healing (Gap Filling)

After range sync, some intermediate trie nodes may be missing. Trie healing fills these gaps:

```
Primary A                              Replica B
    │                                      │
    │  1. "Need nodes: [hash1, hash2...]"  │
    │◄─────────────────────────────────────│
    │                                      │
    │  2. node_data[]                      │
    │─────────────────────────────────────►│
    │                                      │
    │         B processes nodes,           │
    │         discovers more missing       │
    │                                      │
    │  3. "Need nodes: [hash3, hash4...]"  │
    │◄─────────────────────────────────────│
    │         ... until complete ...       │
```

### Phase 3: Verification

Final verification ensures the local trie hash matches the target:

```cpp
// After sync completes
auto cursor = replica_db.cursor();
cursor.first();
hash_t local_root = get_root_hash(cursor);

if (memcmp(local_root, target_root, 32) == 0) {
    // Sync successful!
} else {
    // Something went wrong, need to investigate
}
```

## Implementation Roadmap

### Stage 1: Foundation (Current)
- [x] `_ReplicationTraits` - Enable 32-byte hashes in nodes
- [x] `ReplicatingMapStorage` / `ReplicatingFileStorage` - Storage classes
- [x] Automatic hash computation in `prepare_commit()`
- [ ] Root hash accessor API

### Stage 2: Merkle Proofs
- [ ] `_MerkleProof::prove()` - Single key proof
- [ ] `_MerkleProof::verify()` - Proof verification
- [ ] `_MerkleProof::prove_range()` - Range proof with edges
- [ ] `_MerkleProof::verify_range()` - Range proof verification

### Stage 3: Sync Primitives
- [ ] `_TrieSync` - Node-by-node sync scheduler
- [ ] `_RangeSync` - Bulk range sync with proofs
- [ ] Serialization for node data transfer

### Stage 4: Orchestration
- [ ] `_SyncScheduler` - State machine for sync process
- [ ] Progress tracking and resumption
- [ ] Error handling and retry logic

### Stage 5: Network Protocol
- [ ] Wire protocol for sync messages
- [ ] Peer management
- [ ] Bandwidth throttling

## Wire Protocol (Proposed)

Based on Ethereum's snap protocol:

```
Message Types:
  0x01 GET_ROOT_HASH
  0x02 ROOT_HASH        { hash[32] }
  
  0x10 GET_RANGE        { start_key, end_key, max_bytes }
  0x11 RANGE_DATA       { entries[], proof[], more }
  
  0x20 GET_TRIE_NODES   { hashes[] }
  0x21 TRIE_NODES       { nodes[] }
  
  0x30 SYNC_COMPLETE
  0x31 SYNC_ERROR       { error_code, message }
```

## Performance Considerations

### Why Range Sync First?

From Ethereum's experience:

| Approach | Ethereum Mainnet Sync Time |
|----------|---------------------------|
| Full sync (all nodes) | ~1 week |
| Snap sync (ranges first) | ~12 hours |

Range sync is ~14x faster because:
1. Fewer round trips (bulk data vs. node-by-node)
2. No need to download intermediate trie nodes initially
3. Can parallelize across key ranges

### Optimal Range Size

Based on Ethereum's tuning:
- **Max entries per range**: 10,000 - 50,000
- **Max bytes per response**: 500KB - 2MB
- **Concurrent range requests**: 3-10

### Storage Overhead

| Database Size | Hash Overhead | % Increase |
|--------------|---------------|------------|
| 1 MB | ~32 KB | 3.2% |
| 100 MB | ~3.2 MB | 3.2% |
| 10 GB | ~320 MB | 3.2% |

## Example: Complete Sync Implementation

```cpp
#include <leaves/replicating_mmap.hpp>
#include <leaves/sync.hpp>  // Future header

// Primary node
void primary_server() {
    auto storage = leaves::ReplicatingMapStorage::create("primary.lvs");
    auto db = (*storage)["data"];
    
    // Handle incoming sync requests
    while (auto request = receive_request()) {
        switch (request.type) {
            case GET_ROOT_HASH:
                send(get_root_hash(db));
                break;
                
            case GET_RANGE:
                send(fulfill_range_request(db, request));
                break;
                
            case GET_TRIE_NODES:
                send(get_nodes(db, request.hashes));
                break;
        }
    }
}

// Replica node
void replica_sync() {
    auto storage = leaves::ReplicatingMapStorage::create("replica.lvs");
    auto db = (*storage)["data"];
    
    // Get target root from primary
    hash_t target_root = request_root_hash(primary);
    
    // Initialize sync scheduler
    _SyncScheduler<decltype(db)::InternalDB> scheduler;
    scheduler.start(target_root);
    
    while (!scheduler.complete()) {
        auto action = scheduler.next_action();
        
        switch (action.type) {
            case REQUEST_RANGE: {
                auto response = request_range(primary, action.range_request);
                scheduler.on_range_response(response);
                break;
            }
            case REQUEST_NODES: {
                auto nodes = request_nodes(primary, action.node_hashes);
                scheduler.on_nodes_response(nodes);
                break;
            }
            case APPLY_DATA: {
                auto cursor = db.cursor();
                cursor.begin();
                for (const auto& [k, v] : action.entries) {
                    cursor.insert(k, v);
                }
                cursor.commit();
                break;
            }
        }
    }
    
    // Verify final state
    assert(get_root_hash(db) == target_root);
}
```

## Conflict Resolution

When replicas diverge (different writes to same key):

### Strategy 1: Last-Write-Wins (LWW)
Each entry includes a timestamp; newest wins.

### Strategy 2: Primary-Authoritative
One node is designated primary; conflicts resolve to primary's version.

### Strategy 3: Application-Level
Application provides merge function for conflicts.

```cpp
// Example: Application-level merge
auto merged_value = app.merge_conflict(
    key,
    local_value, local_timestamp,
    remote_value, remote_timestamp
);
```

## Security Considerations

1. **Always verify proofs before applying data** - Never trust remote data
2. **Validate node structure** - Malformed nodes could crash the database
3. **Rate limit sync requests** - Prevent DoS attacks
4. **Authenticate peers** - Only sync with trusted nodes

## Troubleshooting

### Sync Stuck at X%

1. Check network connectivity to peer
2. Verify peer has the target root hash
3. Check for corrupted local data

### Hash Mismatch After Sync

1. Ensure deterministic key ordering
2. Check for concurrent writes during sync
3. Verify hash computation is consistent

### Out of Memory During Sync

1. Reduce batch sizes in `_SyncScheduler`
2. Enable disk-based temporary storage
3. Limit concurrent range requests


## TODOS

- Replication Coordinator
- Delete Database
- Big Value Handlung


