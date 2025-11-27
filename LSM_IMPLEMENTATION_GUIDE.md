# LSM Optimization Implementation Guide for Leaves Database

## Overview

This document describes the LSM (Log-Structured Merge) optimization for the Leaves database, designed to eliminate Copy-On-Write (COW) overhead during burst writes by buffering in MemoryDB before merging to persistent storage.

## Performance Goals

- **Current Performance:** 577.3 MB/s sequential writes, 290.7 MB/s random writes
- **Target Performance:** ~900 MB/s sequential writes, ~450 MB/s random writes with LSM
- **Read Performance:** Maintain 312.1 MB/s random reads, 2515.9 MB/s sequential reads

## Architecture Overview

### Core Concept

1. Writes go to **Segment** (fixed-size 64KB block) first (no COW overhead)
2. When segment is full, allocate new segment and link it to the transaction
3. Background thread merges segments to persistent storage
4. Reads search: newest segment → oldest segment → persistent storage

### Cursor Hierarchy

```
_CursorBase (base navigation, no find/transactions)
    ↓
_Cursor (single-writer, full transactions, fast path - NO CHANGES)
    ↓
_LSMCursor (LSM-aware, multi-source search, transactional, auto-commit)
    ↓
_MultiWriterCursor (concurrent writers, NOT transactional, conflict callbacks)
```

### Key Design Decisions

- **_Cursor remains unchanged** - preserves single-writer fast path (577 MB/s)
- **Segment-level refcounting** - Each segment tracks cursor references
- **Segment allocation strategy** - Carve fixed-size segments from areas, recycle via `_GarbageSlot`
- **Segments are storage blocks** - 64KB blocks in persistent storage, accessible via offsets
- **Atomic area management** - Areas managed with atomic head/tail pointers instead of AreaList objects
  - Header stores head pointers: `area_list_head_single`, `area_list_head_multi`
  - Transactions store tail pointers: `area_list_tail_single`, `area_list_tail_multi`
  - Operations use `add(offset_t head, offset_t tail)` pattern for atomic transfers
  - Storage methods use `return_single_areas(offset_t head, offset_t tail)`
  - No AreaList objects needed, simpler and more efficient
- **Tombstones** - Use bit 14 of `LeafNode::value_size` for delete markers
- **Search order** - Newest to oldest ensures correct semantics
- **Merge order** - Oldest to newest, one at a time (keeps merger simple)
- **Auto-commit** - Configurable timeout (default 200ms) for automatic commits
- **std::function overhead** - No heap allocation during call, only during construction (SBO ~16-32 bytes)

## Implementation Phases

### Phase 1: Core LSM Infrastructure (2-3 weeks)

#### 1.1 Database Architecture Restructuring

**Design Principle:** Keep the original `_DB` and `_Transaction` unchanged. Create new LSM variants that derive from them. Support multi-process access for memory-mapped storage.

**File:** `include/leaves/intern/_db.hpp`

Make `_DB` template-friendly by adding Transaction and Header template parameters:

```cpp
// Original transaction remains unchanged
template <typename Traits_>
struct _Transaction : public _TransactionBase<Traits_> {
    // Existing fields unchanged
};

// Default database header (defined outside _DB)
template <typename Storage_>
struct _DBHeader {
    using Mutex = typename Storage_::Mutex;
    
    offset_t read_txn;
    offset_t prepared_txn;
    Mutex txn_lock;
    std::atomic<uint64_t> txn_cursor_id;
    
    // Atomic area management - no AreaList objects, just head pointers
    // Areas are linked lists in storage, operations use atomic head/tail pattern
    offset_t area_list_head_single;  // head of single AREA_SIZE areas linked list
    offset_t area_list_head_multi;   // head of multi-AREA_SIZE areas linked list
    AreaPool area_pool;              // area pool for allocating areas
};

// Make _DB accept Transaction and Header as template parameters
template <typename Storage_, 
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename Header_ = _DBHeader<Storage_>>
struct _DB {
    typedef Storage_ Storage;
    typedef Transaction_ Transaction;
    typedef Header_ Header;
    using Traits = typename Storage::Traits;
    using txn_ptr = typename Traits::template Pointer<Transaction>;
    // ... rest remains same
};
```

**Key Changes:**
- `_DBHeader<Storage_>` defined outside `_DB` for reusability
- Header is a direct template parameter (no void/conditional logic)
- LSM headers can extend `_DBHeader<Storage_>` with additional fields
- Backward compatible - existing code works with default header
- **Atomic area management:** Areas are now managed with atomic head/tail pointers instead of AreaList objects
  - Header stores `area_list_head_single` and `area_list_head_multi` (head pointers)
  - Transactions store `area_list_tail_single` and `area_list_tail_multi` (tail pointers for pending areas)
  - Operations use `add(offset_t head, offset_t tail)` pattern instead of `move(AreaList&)`
  - Storage methods accept `return_single_areas(offset_t head, offset_t tail)`
  - Eliminates need for AreaList objects in header, simpler and more efficient

**Add Region-Specific Flush Methods for Sync Commits:**

LSM needs to selectively flush specific memory regions (segments, transaction fields, header fields) during sync commits instead of flushing the entire file. Add these methods to support this:

**File:** `include/leaves/intern/_db.hpp`
```cpp
// Add new flush methods that flush specific memory regions
void flush(void* ptr, offset_t offset, size_t size, bool sync = false) {
    _storage.flush(ptr, offset, size, sync);
}
```

**File:** `include/leaves/intern/_mmap.hpp`
```cpp
// Add new flush method for specific memory regions
void flush(void* ptr, offset_t offset, size_t size, bool sync = false) {
    if (size > 0) {
        _region.flush(reinterpret_cast<char*>(ptr) - static_cast<char*>(_region.get_address()),
                     size, !sync);
    }
}
```

**File:** `include/leaves/intern/_cachestore.hpp`
```cpp
// Add new flush method (no-op for cache storage)
void flush(void* ptr, offset_t offset, size_t size, bool sync = false) {
    // Cache storage doesn't need region-specific flushing
    // Existing flush() already handles dirty area management
}
```

**Purpose:** These changes enable LSM's sync commits to selectively flush only the memory regions that need durability (segment data, transaction metadata, header fields) instead of flushing the entire database, significantly improving sync commit performance.

#### 1.2 LSM Transaction Extension and Multi-Process Header

**File:** `include/leaves/intern/_lsm_db.hpp` (new file)

Create LSM-specific transaction, header, and database with **multi-process support**:

```cpp
#include "_db.hpp"
#include "_memory.hpp"
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

namespace leaves {

// Segment-specific traits with smaller BlockContainer
template <typename BaseTraits_>
struct SegmentTraits : public BaseTraits_ {
    static constexpr size_t BLOCK_CONTAINER_SIZE = 512;
};

// LSM Header - extends base DB header with multi-process LSM state
// Lives in shared storage for memory-mapped files
template <typename Storage_>
struct LSMHeader : public _DBHeader<Storage_> {
    using BaseHeader = _DBHeader<Storage_>;
    using Traits = typename Storage_::Traits;
    
    // Note: No merge queue needed! Use existing transaction chain:
    // - read_txn points to last committed transaction
    // - each transaction has next_txn for chaining
    // - transactions with merge_phase != COMMITTED have refs > 0 to prevent freeing
    
    // Last merged transaction ID (for cursor validation)
    std::atomic<tid_t> last_merged_txn_id{0};
    
    // Background thread coordination (multi-process safe)
    boost::interprocess::interprocess_mutex background_mutex;
    boost::interprocess::interprocess_condition background_cv;
    std::atomic<uint8_t> background_running{0};  // 0 = stopped, 1 = running
    std::atomic<uint8_t> background_active{0};   // 0 = no thread, 1 = thread active
    
    // Auto-commit configuration (shared across processes)
    uint32_t auto_commit_ms{200};
    uint8_t auto_commit_enabled{0};
    
    void init() {
        last_merged_txn_id.store(tid_t(0), std::memory_order_release);
        background_running.store(0, std::memory_order_release);
        background_active.store(0, std::memory_order_release);
        auto_commit_ms = 200;
        auto_commit_enabled = 0;
    }
};

// LSM-specific transaction - extends base transaction
template <typename Traits_>
struct _LSMTransaction : public _Transaction<Traits_> {
    // LSM-specific fields
    offset_t segment_head{0};            // Head of segment linked list (newest first)
    offset_t current_segment{0};         // Current segment offset being written
    uint32_t current_segment_bytes{0};   // Bytes used in current segment
    uint8_t merge_phase{WRITING};        // Current merge phase
    int16_t _pool_slot_index{-1};        // Slot index in multi-writer pool (-1 = not using)
    
    // Segment recycling (transaction-local, no mutex needed)
    _GarbageSlot<Traits_> _free_segments;
};
```

**Multi-Process Design:**
- **No separate merge queue needed**: Use existing transaction chain infrastructure
  - `read_txn` in header points to last committed transaction
  - Each transaction has `next_txn` linking to newer transactions
  - Background thread walks chain starting from oldest transaction with `start_txn`
- **Transaction protection during merge**: 
  - Transactions being merged maintain `refs > 0` to prevent premature freeing
  - `start_transaction()` walks transaction chain, frees only transactions with `refs == 0`
  - LSM sets transaction refs before merge, clears after merge completes
- **No allocation overhead**: Zero extra fields or structures needed
- **Background thread ownership**: Atomic `background_active` ensures only one process runs thread
- **Synchronization**: `boost::interprocess::interprocess_mutex` and `interprocess_condition`
- **Last merged txn_id**: Atomic in shared memory for cursor validation across processes
- **Auto-commit config**: Stored in shared header, visible to all processes

**Key Constraint:** Cannot use `std::queue`, `std::mutex`, `std::condition_variable` - they don't work in shared memory!

```cpp
// Segment represents a fixed-size (64KB) storage block for one transaction's writes
// Segments use MemManager with segment-specific traits (512-byte containers) for internal allocation
// Note: Segments do NOT inherit from _MemoryDB - they are lightweight wrappers around MemManager
template <typename LSMDB_>
struct Segment {
    typedef LSMDB_ LSMDB;
    typedef typename LSMDB::Traits Traits;
    typedef _MemManager<Traits> MemManager;
    
    MemManager _mem_manager;
    tid_t txn_id;
    offset_t allocation_start{0};
    offset_t next{0};                // Next segment in the linked list (offset, not pointer)
    std::atomic<int> refs{0};        // Refcount tracks all cursors using this segment
    LSMDB* parent_db{nullptr};       // Parent LSMDB for forwarding big memory operations
    
    static constexpr uint32_t SEGMENT_SIZE = 65536; // 64KB total
    
    // Initialize segment's memory manager with specific bounds
    // Called once when area is split into segments
    void init(offset_t data_start, offset_t data_end) {
        allocation_start = data_start + sizeof(Segment);
        _mem_manager.init(allocation_start, data_end);
        next = 0;
        refs.store(0, std::memory_order_release);
    }
    
    // Set parent DB pointer before returning segment to user
    void reinit(LSMDB* parent) {
        _mem_manager.allocation_start = allocation_start;
        refs.store(1, std::memory_order_release);
        next = 0;
        parent_db = parent;
    }
    
    // Allocate memory within this segment
    template <typename Resolver>
    auto alloc(uint16_t slot_id, Resolver& resolver) {
        return _mem_manager.alloc(slot_id, resolver);
    }
    
    // Cursor management - single refs field for simplicity
    void acquire() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() { refs.fetch_sub(1, std::memory_order_release); }
    
    bool can_recycle() const { 
        return refs.load(std::memory_order_acquire) == 0; 
    }
};

enum MergePhase : uint8_t {
    WRITING = 0,      // Active transaction, writes going to segment
    COMMITTING = 1,   // CommEitted, ready for merge
    COMMITTED = 2     // Merge complete, transaction fully committed
};

// LSM-specific transaction - extends base transaction
template <typename Traits_>
struct _LSMTransaction : public _Transaction<Traits_> {
    typedef _Transaction<Traits_> BaseTransaction;
    
    // LSM-specific fields
    offset_t segment_head{0};            // Head of segment linked list (oldest first)
    offset_t current_segment{0};         // Current segment offset being written
    uint32_t current_segment_bytes{0};   // Bytes used in current segment
    uint8_t merge_phase{WRITING};
    uint8_t sync_commit{0};              // 1 = committed with sync, requires sync flush after merge
    
    // Segment recycling via _GarbageSlot (fixed 64KB segments)
    _GarbageSlot<Traits_> _free_segments;
    
    static constexpr uint32_t SEGMENT_SIZE = 65536; // 64KB per segment
    static constexpr uint32_t SEGMENT_DATA_SIZE = SEGMENT_SIZE - sizeof(Segment); // Usable space
    
    // Override size to include LSM fields
    uint16_t size() const { 
        return sizeof(_LSMTransaction); 
    }
    
    // Segment allocation - transaction member
    template <typename DB>
    offset_t _allocate_segment(DB* db);
};

// LSM Database - derives from base DB
template <typename Storage_>
struct _LSMDB : public _DB<Storage_, _LSMTransaction<typename Storage_::Traits>, LSMHeader<Storage_>> {
    typedef _DB<Storage_, _LSMTransaction<typename Storage_::Traits>, LSMHeader<Storage_>> BaseDB;
    typedef _LSMTransaction<typename Storage_::Traits> Transaction;
    using txn_ptr = typename BaseDB::txn_ptr;
    using Traits = typename Storage_::Traits;
    
    // Note: No _merge_queue needed - use existing transaction chain via iter_transactions()
    // Background thread state is in LSMHeader for multi-process support
    
    _LSMDB(Storage_& storage, uint16_t index) : BaseDB(storage, index) {}
    
    ~_LSMDB() {
        _stop_background_thread();
    }
    
    // Auto-commit API (configuration stored in LSMHeader)
    void enable_auto_commit(uint32_t timeout_ms = 200) {
        auto* header = _get_lsm_header();
        header->auto_commit_enabled = 1;
        header->auto_commit_ms = timeout_ms;
    }
    
    void disable_auto_commit() {
        auto* header = _get_lsm_header();
        header->auto_commit_enabled = 0;
    }
    
    // Helper to access LSMHeader from base class
    LSMHeader<Storage_>* _get_lsm_header() {
        return static_cast<LSMHeader<Storage_>*>(this->_header);
    }
    
    // Background thread operations
    void _start_background_thread();
    void _stop_background_thread();
    void _background_loop();
    
    // LSM operations performed by background thread
    void _merge_transaction(txn_ptr& txn);
    void _prune_tombstones_cycle();
    void _check_auto_commit();
    
    // Override start_transaction to allocate initial segment
    void start_transaction() {
        BaseDB::start_transaction();
        
        auto& lsm_txn = static_cast<Transaction&>(*this->_txn);
        
        // Allocate first segment
        lsm_txn.current_segment = lsm_txn._allocate_segment(this);
        lsm_txn.segment_head = lsm_txn.current_segment;
        lsm_txn.merge_phase = WRITING;
        
        // Initialize segment
        Segment* segment = this->resolve(lsm_txn.current_segment, WRITE);
        segment->init(lsm_txn.current_segment, this);
        segment->txn_id = lsm_txn.txn_id;
        this->make_dirty(segment);
        
        // Note: Transaction is automatically added to chain by BaseDB
        // Background thread will discover it via iter_transactions()
    }
    
    // Override rollback - _GarbageSlot rollback handles segment cleanup
    void rollback() {
        // Transaction rollback automatically restores _free_segments to previous state
        // All segments allocated during transaction are returned to pool
        BaseDB::rollback();
    }
    
    // Override prepare_commit to flush segments in sync mode
    tid_t prepare_commit(uint64_t cursor_id, bool sync = false) {
        // Call base prepare_commit to update prepared_txn and next_txn
        tid_t result = BaseDB::prepare_commit(cursor_id, false);

        auto& lsm_txn = static_cast<Transaction&>(*this->_txn);
        
        // In sync mode, flush segments before preparing transaction
        // This ensures segment data is durable before the transaction becomes visible
        if (result && sync) {
            // Flush all segments in this transaction
            offset_t segment_offset = lsm_txn.segment_head;
            while (segment_offset) {
                Segment* segment = this->resolve(segment_offset, WRITE);
                
                // Flush only the used portion (allocation_start to allocation_end)
                offset_t start = segment->mem_manager.allocation_start;
                offset_t end = segment->mem_manager.allocation_end;
                size_t used_size = end - start;
                
                this->flush(segment, start, used_size, true);  // sync=true
                segment_offset = segment->next;
            }
            
            // Flush prepared_txn pointer, read_txn->next_txn, and the whole _wtxn
            this->flush(this->_header, offsetof(Header, prepared_txn), 
                       sizeof(offset_t), true);  // prepared_txn field

            txn_ptr read_txn = this->resolve(this->_header->read_txn);
            this->flush(read_txn.get(), offsetof(Transaction, next_txn),
                       sizeof(offset_t), true);  // next_txn field
            
            this->flush(this->_wtxn.get(), 0, lsm_txn.size(), true);  // whole transaction
        }
        
        return result;
    }
    
    // Override commit to mark transaction as ready for merge
    void commit(bool sync = false) {
        auto& lsm_txn = static_cast<Transaction&>(*this->_txn);
        
        // Increment refs to prevent transaction from being freed during merge
        lsm_txn.refs.fetch_add(1, std::memory_order_release);
        
        // Change phase to COMMITTING - background thread will process it
        lsm_txn.merge_phase = COMMITTING;
        
        // Record if this was a sync commit for later flush decision
        lsm_txn.sync_commit = sync ? 1 : 0;
        
        // Notify background thread that a transaction is ready
        auto* header = _get_lsm_header();
        header->background_cv.notify_one();
        
        // Commit transaction metadata (always with sync=false)
        // In sync mode, prepare_commit already flushed everything
        BaseDB::commit(false);

        if (sync) {
            // Flush read_txn (last committed transaction)
            this->flush(this->_header, offsetof(Header, read_txn), 
                        sizeof(offset_t), true);
        }

        // Note: Don't wait for merge - background thread will:
        // 1. Merge segments to persistent storage
        // 2. Set merge_phase to COMMITTED
        // 3. Do synchronized flush if sync_commit flag is set
        // 4. Release refs to allow segment freeing
    }
};

} // namespace leaves
```

#### 1.3 Segment Internal Memory Management

**Design Decision: Use MemManager for Variable-Size Trie Node Allocation**

Segments host **full trie structures** that are modified during transactions. The `_Inserter` allocates and modifies trie nodes (TrieNode, LeafNode) using copy-on-write semantics, which creates garbage from old node versions.

**Key Reality:**
- Segments contain trie structures, not linear data
- `_Inserter` performs COW operations: node splits, trie level creation, updates
- Variable node sizes: small LeafNodes (~20-100 bytes) vs large TrieNodes (100-1000+ bytes)
- During transaction: allocate → modify (COW copy) → old version becomes garbage
- Within transaction: freed nodes should be recycled
- 64KB size provides good balance: cache-friendly, low overhead, efficient MemManager usage

**Rationale for MemManager:**
- **Variable sizes** - TrieNodes vary dramatically in size, need size-class slots
- **COW creates garbage** - Many updates = many old node versions to recycle
- **Recycling critical** - Long transactions with many updates need to reuse freed space
- **Proven infrastructure** - Already handles transaction-safe recycling via `_GarbageSlot`
- **Natural fit** - Segment acts as a mini-area with MemManager managing its 64KB

**Segment-Specific Traits:**

The default `_BlockContainer` size (4KB) is too large for 64KB segments. We configure a smaller container via traits:

```cpp
// Segment traits - optimized for 64KB mini-database
template <typename BaseTraits_>
struct SegmentTraits : public BaseTraits_ {
    typedef BaseTraits_ BaseTraits;
    
    // Smaller BlockContainer for segment's limited space
    // 512 bytes instead of 4KB - enough for ~60 freed block entries
    // The _BlockContainer template in _memory.hpp uses this to determine its size
    static constexpr size_t BLOCK_CONTAINER_SIZE = 512;
};
```

**How _BlockContainer Adaptation Works:**

The `_BlockContainer` template in `_memory.hpp` has been made configurable:

```cpp
// Helper to get BLOCK_CONTAINER_SIZE from Traits, default to 4K if not present
template <typename Traits, typename = void>
struct _GetBlockContainerSize {
  static constexpr uint16_t value = 4 * K;  // Default 4KB
};

template <typename Traits>
struct _GetBlockContainerSize<Traits, std::void_t<decltype(Traits::BLOCK_CONTAINER_SIZE)>> {
  static constexpr uint16_t value = Traits::BLOCK_CONTAINER_SIZE;  // Use trait if present
};

template <typename Traits>
struct _BlockContainer : public Traits::BlockHeader {
  static constexpr uint16_t SIZE = _GetBlockContainerSize<Traits>::value;
  static constexpr uint16_t SLOT_ID = 
      binary_search(&BLOCK_SIZES[0], &BLOCK_SIZES[BLOCK_SIZES_COUNT], SIZE);
  // ... rest of implementation
};
```

This approach:
- **No code duplication** - Single `_BlockContainer` implementation works for all sizes
- **SFINAE detection** - Automatically detects if `BLOCK_CONTAINER_SIZE` is defined
- **Dynamic slot ID** - Uses `binary_search()` to find appropriate slot based on size
- **Backward compatible** - Defaults to 4KB for traits that don't specify a size

**Memory Efficiency:**

With 512-byte containers (configured via `BLOCK_CONTAINER_SIZE = 512`), segments achieve 99% efficiency:
- Segment: 64KB total
- Header: ~40 bytes (next, txn_id, refs)
- MemManager fields: ~50 bytes (slots array, pointers)
- BlockContainer (worst case): ~512 bytes (only allocated when needed)
- Usable data: ~65000 bytes (99% efficiency even with one container allocated)

**Write Path Integration:**

_Inserter uses `cursor->_db` to allocate. For LSMCursor, the cursor's `_db` points to current Segment (which is a _MemoryDB).

```cpp
// Segment overflow detection in alloc_single_area
template <typename LSMDB_>
offset_t Segment<LSMDB_>::alloc_single_area(uint32_t size) {
    // Segment is full - cannot allocate more areas
    // Return 0 to signal overflow to MemManager
    return 0;
}

// MemManager detects overflow when alloc_single_area returns 0
// This propagates up to _Inserter as nullptr return from alloc()

// _Inserter's exec() returns false on overflow
template <typename Cursor_>
bool _Inserter<Cursor_>::exec() {
    // ... insertion logic ...
    
    // Allocate new node
    auto new_node = cursor->_db->alloc(node_size);
    
    if (!new_node) {
        // Segment overflow - signal to cursor
        return false;
    }
    
    // Continue with insertion...
    return true;
}

// LSMCursor adds current transaction's segment at index 0
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_add_current_segment() {
    if (!_base_cursor._txn) return;
    
    auto& lsm_txn = *_base_cursor._txn;
    
    if (lsm_txn.current_segment) {
        Segment* segment = _base_cursor._db->template resolve<Segment>(lsm_txn.current_segment, WRITE);
        
        segment->acquire();
        // Insert at index 0 (newest segment for writes)
        auto cursor = std::make_unique<SegmentCursor>(segment);
        _segment_cursors.insert(_segment_cursors.begin(), std::move(cursor));
    }
}

// Override reserve to handle segment overflow
template <typename DB_, typename Traits_>
void* _LSMCursor<DB_, Traits_>::reserve(size_t size) {
    // Start transaction if not already started - creates first segment
    [[maybe_unused]] bool r = start_transaction();
    assert(r);
    
    // Infinite retry loop - always succeeds by creating new segments
    while (true) {
        auto& segment_cursor = _segment_cursors[0];
        if (_active_cursor != segment_cursor.get()) {
            segment_cursor->find(key());
            _active_cursor = segment_cursor.get();
        }
        
        void* result = _active_cursor->reserve(size);
        if (result) return result;
        
        // Segment overflow - create new segment and retry
        auto& lsm_txn = *_base_cursor._txn;
        _base_cursor._db->add_segment(_base_cursor._txn.get());
        _add_current_segment();
        // Retry reserve with new segment (will succeed)
    }
}

// Note: value() uses reserve(), so it also handles overflow automatically
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);  // reserve() loops until success
    memcpy(space, value.data(), value.size());
}
```

**Merge Path:**
```cpp
// _Merger walks trie structure starting from root
template <typename Storage_>
void _LSMDB<Storage_>::_merge_one_segment(Segment* segment, Transaction* txn) {
    // Note: Merge state tracked by txn->merge_phase, not per-segment
    
    // Create destination cursor (persistent storage)
    Cursor dst_cursor(this);
    
    // Create source cursor for segment's trie root
    offset_t segment_root = segment->get_trie_root();
    _NodeIterator<Segment, Traits> src_iterator(segment, segment_root);
    
    // Merge trie structures
    LSMergeHandler handler;
    _Merger<Cursor, decltype(src_iterator), LSMergeHandler> merger(
        dst_cursor, src_iterator, handler
    );
    
    merger.exec();
    
    // Transaction's merge_phase will be set to COMMITTED after all segments merged
    // ... recycling logic
}
```

**Rejected Alternative (Arena-only):**
- **Simple append-only** - Would waste space on COW garbage, fill segments faster
- **Why rejected:** Transactions can have many updates, COW creates significant garbage

**Memory Usage:**
- Segment size: 64KB fixed (chosen for cache-friendliness and low overhead)
- Header: ~32 bytes (next, txn_id, refs, parent_db pointer)
- MemManager overhead: ~50 bytes (slots array, allocation pointers)
- BlockContainer: 512 bytes (only allocated when needed for freed block tracking, ~0.8% overhead)
- Usable data: ~64.5-65KB (99%+ efficiency depending on recycling activity)
- Benefits: Reuses freed nodes, handles variable sizes efficiently, right-sized container, excellent overhead ratio
- Note: Merge state tracked at transaction level (merge_phase), not per-segment

#### 1.4 Segment Allocation and Recycling

**Key Insight:** Segments are self-contained 64KB storage blocks with internal MemManager. Each segment is allocated from persistent storage and recycled as a complete unit using `_GarbageSlot` infrastructure. Segments track usage via a single `refs` field (simplified from separate cursor_refs).

```cpp
// Segment allocation - implemented as transaction member method
template <typename Traits_>
template <typename DB>
offset_t _LSMTransaction<Traits_>::_allocate_segment(DB* db) {
    // Create segment resolver for this transaction
    SegmentResolver<DB> resolver(db);

    // Try to pop from recycle pool (transaction-safe)
    auto segment_block_ptr = _free_segments.pop(resolver);
    if (segment_block_ptr) {
        // Reuse existing segment from pool (already initialized)
        offset_t segment_offset = db->resolve(segment_block_ptr);
        auto segment_ptr =
            db->template resolve<typename DB::SegmentType>(segment_offset, WRITE);

        // Reset segment state
        segment_ptr->reinit(db);

        // Immediately push back to _GarbageSlot for future recycling
        // SegmentResolver::may_recycle() will check refs and merge_phase
        // when popping
        _free_segments.push(segment_block_ptr, resolver);

        return segment_offset;
    }

    // Allocate new area from storage if pool empty
    // Split area into segments, last segment gets remaining space
    auto area = db->alloc_single_area();
    offset_t area_start = area->content_offset();
    offset_t area_end = area->end();

    // Split area into segments and push all to _free_segments
    offset_t seg_offset = area_start;
    while (seg_offset < area_end) {
        auto seg_ptr =
            db->template resolve<typename DB::SegmentType>(seg_offset, WRITE);

        // Calculate data bounds for this segment
        // data_start accounts for Segment header (includes BaseMemoryDB fields +
        // LSM fields)
        offset_t data_start = seg_offset;
        offset_t data_end;

        if (seg_offset + SEGMENT_SIZE <= area_end) {
            // Full-sized segment
            data_end = seg_offset + SEGMENT_SIZE;
        } else {
            // Last segment takes remaining space
            data_end = area_end;
        }

        // Initialize segment with specific bounds (called only once)
        seg_ptr->init(data_start, data_end);
        db->make_dirty(seg_ptr);
        _free_segments.push(seg_ptr, resolver);

        seg_offset += SEGMENT_SIZE;
    }

    // Now pop one segment to return to caller
    auto new_segment_block_ptr = _free_segments.pop(resolver);
    offset_t new_segment_offset = db->resolve(new_segment_block_ptr);
    auto new_segment_ptr = db->template resolve<typename DB::SegmentType>(
        new_segment_offset, WRITE);
    new_segment_ptr->reinit(db);
    return new_segment_offset;
}

// Custom resolver for segment recycling
// _GarbageSlot uses this to resolve BlockItem.link to Segment and check refs == 0
template <typename LSMDB_>
struct SegmentResolver {
    typedef LSMDB_ LSMDB;
    typedef typename LSMDB::SegmentType Segment;
    typedef typename LSMDB::Traits Traits;
    typedef typename Traits::template Pointer<Segment> block_ptr;
    typedef _BlockContainer<Traits> BlockContainer;
    typedef typename BlockContainer::ptr cont_ptr;
    typedef typename LSMDB::Transaction Transaction;
    
    LSMDB* db;
    
    SegmentResolver(LSMDB* db_) : db(db_) {}
    
    // Resolve BlockItem.link offset to Segment pointer
    block_ptr resolve(offset_t offset, Access mode) {
        return db->template resolve<Segment>(offset, mode);
    }
    
    template <typename Pointer>
    offset_t resolve(const Pointer& p) const {
        return db->resolve(p);
    }
    
    // Check if segment can be recycled (refs == 0)
    // Note: Simplified from guide - only refs check needed, no merge_phase check
    template <typename BlockItem>
    bool may_recycle(const BlockItem& item) {
        auto segment = db->template resolve<Segment>(item.link, READ);
        return segment->can_recycle();
    }
    
    // Mark segment for recycling with current transaction ID
    template <typename BlockItem>
    void mark_for_recycle(BlockItem& item) {
        // No marking needed - segment state tracked by transaction merge_phase
    }
    
    // Free a BlockContainer back to the database
    void free(cont_ptr block) {
        db->free(block);
    }
    
    // Allocate a BlockContainer for the GarbageSlot
    cont_ptr alloc_slot(uint16_t slot_id) {
        return db->alloc_slot(slot_id);
    }
    
    void make_dirty(cont_ptr block) {
        db->make_dirty(block);
    }
};

// Note: The existing _GarbageSlot in _memory.hpp already supports custom resolvers.
// No changes to _memory.hpp are needed - it's already generic enough for LSM.
// The SegmentResolver above implements the required interface:
//   - resolve() returns segment pointers
//   - may_recycle() checks refs == 0
//   - mark_for_recycle(), free(), alloc_slot(), make_dirty() handle lifecycle

// Segment allocation uses SegmentResolver
template <typename Traits_>
template <typename DB>
offset_t _LSMTransaction<Traits_>::_allocate_segment(DB* db) {
    // Create segment resolver for this transaction
    SegmentResolver<DB> resolver(db, this);
    
    // Try to pop from recycle pool (transaction-safe)
    auto segment_ptr = _free_segments.pop(resolver);
    if (segment_ptr) {
        // Reuse existing segment from pool
        offset_t segment_offset = segment_ptr->offset();
        
        // Initialize segment
        segment_ptr->init(segment_offset, db);  // Pass parent LSMDB pointer
        segment_ptr->next = 0;
        segment_ptr->txn_id = this->txn_id;
        segment_ptr->refs.store(0);
        
        // Immediately push back to _GarbageSlot for future recycling
        // SegmentResolver::may_recycle() will check refs when popping
        _free_segments.push(segment_ptr, resolver);
        
        return segment_offset;
    }
    
    // Allocate new segment from storage if pool empty
    // Carve 64KB block from an area (32 segments per 2MB area)
    auto segment = db->template alloc_block<Segment>(Segment::SEGMENT_SIZE);
    segment->init(segment->offset(), db);  // Pass parent LSMDB pointer
    segment->txn_id = this->txn_id;
    db->make_dirty(segment);
    
    // Push to _GarbageSlot for recycling tracking
    _free_segments.push(segment, resolver);
    
    return segment->offset();
}
```

**Architecture Benefits:**
- **Self-contained segments** - Each 64KB block manages its own space with MemManager
- **Simple allocation** - Carve fixed-size blocks from areas (32 segments per 2MB area)
- **Auto-recycling** - Segments automatically recycle via `release()` when refcount hits 0
- **Transaction-safe** - `_GarbageSlot` integrated into release mechanism
- **Efficient reuse** - Segment pool eliminates allocation overhead

#### 1.4 Tombstone Support

**File:** `include/leaves/intern/_node.hpp`

Modify `LeafNode` to use bit 14 for tombstone flag:

```cpp
template <typename Traits>
struct _LeafNode : public Traits::BlockHeader {
    // Existing fields...
    uint16_e value_size;  // Bit 15 = BIG_VALUE_FLAG, Bit 14 = tombstone flag
    
    static constexpr auto TOMBSTONE_FLAG = uint16_e(1) << 14;
    
    // Helper methods
    bool is_tombstone() const { 
        return (value_size & TOMBSTONE_FLAG) != 0; 
    }
    
    void set_tombstone() { 
        value_size |= TOMBSTONE_FLAG;
        // Note: Tombstone leaves typically have zero-length value
        // The flag alone marks deletion, value_size low bits are 0
    }
    
    void clear_tombstone() {
        value_size &= ~TOMBSTONE_FLAG;
    }
    
    uint16_t get_actual_value_size() const { 
        // Mask out both BIG_VALUE_FLAG (bit 15) and TOMBSTONE_FLAG (bit 14)
        return value_size & 0x3FFF;  // Bits 0-13 only (max 16383 bytes)
    }
    
    void set_value_size(uint16_t size) {
        // Preserve tombstone and big value flags
        value_size = (value_size & (BIG_VALUE_FLAG | TOMBSTONE_FLAG)) | (size & 0x3FFF);
    }
    
    // Update vsize() to mask tombstone flag
    uint16_t vsize() const { 
        return value_size & ~(BIG_VALUE_FLAG | TOMBSTONE_FLAG); 
    }
};
```

**Note:** Small values can now be up to 16383 bytes (14 bits), which is sufficient since the configurable threshold is typically 16KB and the actual max per-node size is limited by block sizes.

#### 1.5 Write Path Modification

**File:** `include/leaves/intern/_inserter.hpp`

**Required Changes:** `_Inserter::exec()` must return false when segment overflows (alloc returns nullptr).

```cpp
template <typename Cursor_>
class _Inserter {
    // Existing insertion logic...
    
    // exec() must check allocation results and return false on failure
    bool exec() {
        // ... existing insertion logic ...
        
        auto new_node = cursor->_db->alloc(node_size);
        if (!new_node) {
            // Allocation failed - abort insertion
            return false;
        }
        
        // ... continue with insertion ...
        return true;
    }
};
```

**File:** `include/leaves/intern/_cursor.hpp`

**Required Changes:** `_Cursor::reserve()` must check `_Inserter::exec()` return value and return nullptr on failure. Use CRTP to allow derived classes to override `reserve()` while keeping `value()` in base class.

```cpp
// CRTP-enabled cursor - Derived_ defaults to _Cursor itself for non-CRTP usage
template <typename Derived_, typename DB_, typename Traits_>
struct _CursorCRTP : public _CursorBase<DB_, Traits_> {
    // ... existing methods ...
    
    void* reserve(size_t size) {
        [[maybe_unused]] bool r = start_transaction();
        assert(r);

        if (!this->stack.size) {
            if (!Traits::get_root(this->_txn)) {
                this->push(offset_t());
                if (!_Inserter(&this->stack.back(), size).first_exec()) {
                    return nullptr;  // Allocation failed
                }
                return (void*)this->stack.back().value().data();
            }
            throw NoValidPosition();
        }

        if (!_Inserter(&this->stack.back(), size).exec()) {
            return nullptr;  // Allocation failed (e.g., segment overflow)
        }
        return (void*)this->stack.back().value().data();
    }
    
    void value(const Slice& value) {
        // CRTP: Call derived class's reserve() method
        void* space = static_cast<Derived_*>(this)->reserve(value.size());
        if (!space) {
            throw std::bad_alloc();  // Real allocation failure
        }
        memcpy(space, value.data(), value.size());
        this->_db->flush();
    }
};

// Regular _Cursor that uses itself as Derived (no CRTP override)
template <typename DB_, typename Traits_>
struct _Cursor : public _CursorCRTP<_Cursor<DB_, Traits_>, DB_, Traits_> {
    using Base = _CursorCRTP<_Cursor<DB_, Traits_>, DB_, Traits_>;
    using Base::Base;  // Inherit constructors
};
```

**Impact:**
- `_Inserter::exec()` returns false when allocation fails (segment overflow or OOM)
- `_Cursor::reserve()` checks `_Inserter::exec()` return value and returns nullptr on failure
- `_Cursor::value()` checks reserve() return and throws std::bad_alloc on nullptr
- `_LSMCursor::reserve()` wraps segment cursor's reserve(), handles nullptr by allocating new segment and retrying

The actual LSM write path changes occur in:
- **_Inserter**: Handle nullptr from alloc(), trigger segment overflow
- **Transaction layer**: Route writes to current segment instead of persistent storage
- **Database layer**: Allocate new segment when current is full
- **Cursor layer** (Phase 2): Multi-source search, tombstone handling, and remove() override
  - **remove()**: Inserts tombstone instead of deleting (enables delete batching in segments)
  - **Tombstone format**: Empty value with tombstone flag set in LeafNode
  - **Navigation**: Automatically skips tombstones during iteration

### Phase 2: _LSMCursor Implementation (2-3 weeks)

#### 2.1 LSM Cursor Structure

**File:** `include/leaves/intern/_lsm_cursor.hpp` (new file)

Create LSM-aware cursors that work with `_LSMDB`:

```cpp
#include "_cursor.hpp"
#include "_lsm_db.hpp"

namespace leaves {

// LSM-aware cursor with multi-source search and tombstone handling
// Uses composition pattern: contains _base_cursor instead of inheriting
template <typename DB_, typename Traits_>
struct _LSMCursor {
    typedef DB_ DB;
    typedef Traits_ Traits;
    typedef _Cursor<DB_, Traits_> BaseCursor;
    typedef typename DB::SegmentType Segment;
    typedef typename Segment::Traits SegmentTraits;
    typedef _Cursor<Segment, SegmentTraits> SegmentCursor;
    using txn_ptr = typename DB::txn_ptr;
    using db_ptr = typename Traits::db_ptr;
    
    // Persistent storage cursor (composition instead of inheritance)
    BaseCursor _base_cursor;
    
    // Per-segment cursors (newest to oldest, reverse order from segment_head list)
    // Index 0 is always the current transaction's segment (for writes)
    std::vector<std::unique_ptr<SegmentCursor>> _segment_cursors;
    tid_t _segment_refs_txn_id{0};  // Transaction ID when segment references were acquired
    
    // Active cursor for delegation
    // nullptr = use _base_cursor (persistent storage)
    // non-null = points to segment cursor from _segment_cursors
    SegmentCursor* _active_cursor{nullptr};
    
    // Lazy synchronization flag: set by find(), cleared by next/prev
    // Defers positioning non-active cursors until navigation occurs
    bool _needs_cursor_sync{false};
    
    _LSMCursor(db_ptr db) : _base_cursor(db) {
        _acquire_segment_refs();
    }
    
    ~_LSMCursor() {
        _release_segment_refs();
    }
    
    // Search all sources (segments + persistent storage)
    void find(const Slice& key);
    
    // Check validity (handles tombstones)
    bool is_valid() const;
    
    // Accessors
    Slice key() const;
    Slice value() const;
    
    // Navigation methods (skip tombstones, merge multiple sources using linear scan)
    void first();
    void last();
    void next();
    void prev();
    
    // Write operations (handle segment overflow)
    void value(const Slice& value);
    void* reserve(size_t size);
    void remove();  // Insert tombstone instead of deleting
    
    // Start transaction and add current segment for writes
    bool start_transaction(bool non_blocking = false);
    
    // Private methods (prefixed with _)
    void _check_segment_refs_validity();
    void _acquire_segment_refs();
    void _release_segment_refs();
    void _add_current_segment();
    void _sync_cursors_after_find();  // Lazy cursor synchronization after find()
    void _find_min_cursor();  // Linear scan to find minimum key (forward iteration)
    void _find_max_cursor();  // Linear scan to find maximum key (backward iteration)
};
```

#### 2.2 Tombstone-Aware is_valid() and Multi-Source Find

```cpp
// is_valid() delegates to active cursor (_base_cursor if nullptr)
template <typename DB_, typename Traits_>
bool _LSMCursor<DB_, Traits_>::is_valid() const {
    if (_active_cursor) {
        return _active_cursor->is_valid();
    }
    return _base_cursor.is_valid();
}

// key() and value() also delegate
template <typename DB_, typename Traits_>
Slice _LSMCursor<DB_, Traits_>::key() const {
    return _active_cursor ? _active_cursor->key() : _base_cursor.key();
}

template <typename DB_, typename Traits_>
Slice _LSMCursor<DB_, Traits_>::value() const {
    return _active_cursor ? _active_cursor->value() : _base_cursor.value();
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::find(const Slice& key) {
    _check_segment_refs_validity();
    
    // Set lazy sync flag - defers positioning other cursors until next/prev
    _needs_cursor_sync = true;
    
    // Search segment cursors from newest to oldest (reverse order)
    for (auto& cursor : _segment_cursors) {
        cursor->find(key);
        if (cursor->is_valid()) {
            _active_cursor = cursor.get();
            return;
        }
    }
    
    // Not found in segments, search persistent storage using _base_cursor
    _base_cursor.find(key);
    _active_cursor = nullptr;  // nullptr = use _base_cursor for persistent storage
}

**Note:** `find()` sets `_needs_cursor_sync = true` to enable lazy synchronization. 
This defers positioning all other cursors until `next()` or `prev()` is called, 
avoiding unnecessary work if the user only needs the value at the found key.

```cpp
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);  // reserve() loops until success
    memcpy(space, value.data(), value.size());
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::remove() {
    // LSM cursors never delete - they insert tombstones instead
    // This allows deletes to be merged with writes in segments
    
    if (!is_valid()) throw NoValidPosition();
    
    [[maybe_unused]] bool r = start_transaction();
    assert(r);
    
    // Get the current key
    Slice current_key = key();
    
    // Insert tombstone into current segment (index 0)
    auto& segment_cursor = _segment_cursors[0];
    if (_active_cursor != segment_cursor.get()) {
        segment_cursor->find(current_key);
    }
    
    segment_cursor->insert(current_key, Slice());  // Empty value
    auto* leaf = segment_cursor->leaf();
    assert(leaf);
    leaf->set_tombstone();  // Mark as tombstone
}
```


#### 2.3 Tombstone-Aware Navigation with Linear Scan

**Design Decision:** Original design used priority queues (min-heap/max-heap) for cursor merging, 
but this was over-engineered for the typical LSM use case where N (number of segments) is small (1-5).

**Why Linear Scan is Better:**

- **Simplicity:** ~200 lines of code removed (priority queues, comparators, direction tracking, rebuild logic)
- **Performance:** For N=1-5, linear O(N) scan is faster than O(log N) heap operations
  - No heap allocations/reallocations
  - Better cache locality (linear array scan vs tree structure)
  - Branch predictor friendly (simple loops)
- **Correctness:** Simpler logic means fewer edge cases and bugs

**Implementation Strategy:**

1. **`_find_min_cursor()`:** Linear scan to find cursor with minimum key (forward iteration)
2. **`_find_max_cursor()`:** Linear scan to find cursor with maximum key (backward iteration)
3. **Tombstone skipping:** Integrated directly into `get_best` lambda
4. **Duplicate handling:** After finding best cursor, advance all others at same key

**Lazy Synchronization After find():**

When `find()` is called, only the key lookup is performed. All other cursors remain at their 
previous positions. The `_needs_cursor_sync` flag is set to defer positioning all cursors 
until `next()` or `prev()` is called. This optimization avoids unnecessary work if the user 
only needs the value at the found key.

```cpp
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::first() {
    _check_segment_refs_validity();
    _needs_cursor_sync = false;
    
    // Fast path: No segments active (common case)
    if (_segment_cursors.empty()) {
        _base_cursor.first();
        return;
    }
    
    // Position all cursors at first key
    for (auto& cursor : _segment_cursors) {
        cursor->first();
    }
    _base_cursor.first();
    
    // Find minimum key cursor (handles tombstones and duplicates)
    _find_min_cursor();
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::last() {
    _check_segment_refs_validity();
    _needs_cursor_sync = false;
    
    // Fast path: No segments active (common case)
    if (_segment_cursors.empty()) {
        _base_cursor.last();
        return;
    }
    
    // Position all cursors at last key
    for (auto& cursor : _segment_cursors) {
        cursor->last();
    }
    _base_cursor.last();
    
    // Find maximum key cursor (handles tombstones and duplicates)
    _find_max_cursor();
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::next() {
    _check_segment_refs_validity();
    
    if (!is_valid()) return;
    
    // Fast path: No segments active (common case)
    if (_segment_cursors.empty()) {
        _base_cursor.next();
        return;
    }
    
    // Lazy synchronization: position other cursors if needed
    _sync_cursors_after_find();
    
    // Advance the active cursor
    if (_active_cursor) {
        _active_cursor->next();
    } else {
        _base_cursor.next();
    }
    
    // Find new minimum key cursor
    _find_min_cursor();
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::prev() {
    _check_segment_refs_validity();
    
    if (!is_valid()) return;
    
    // Fast path: No segments active (common case)
    if (_segment_cursors.empty()) {
        _base_cursor.prev();
        return;
    }
    
    // Lazy synchronization: position other cursors if needed
    _sync_cursors_after_find();
    
    // Advance the active cursor backward
    if (_active_cursor) {
        _active_cursor->prev();
    } else {
        _base_cursor.prev();
    }
    
    // Find new maximum key cursor
    _find_max_cursor();
}

// Helper: Lazy cursor synchronization after find()
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_sync_cursors_after_find() {
    if (!_needs_cursor_sync) return;
    
    _needs_cursor_sync = false;
    Slice current_key = key();
    
    // Position all other cursors at the same key
    for (auto& cursor : _segment_cursors) {
        if (cursor.get() != _active_cursor) {
            cursor->find(current_key);
        }
    }
    
    // Also sync base cursor if active cursor is a segment
    if (_active_cursor) {
        _base_cursor.find(current_key);
    }
}

// Helper: Find cursor with minimum key (for forward iteration)
// Uses get_best lambda that skips tombstones and tracks best key
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_find_min_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;
    
    // Lambda: advance cursor past tombstones, return cursor if it has best key
    auto get_best = [&](auto* cursor) {
        while (cursor->is_valid()) {
            auto* leaf = cursor->leaf();
            if (!leaf || !leaf->is_tombstone()) {
                Slice key = cursor->key();
                if (!has_best || key < best_key) {
                    best_key = key;
                    has_best = true;
                    return cursor;
                }
                return nullptr;  // Not best
            }
            cursor->next();  // Skip tombstone
        }
        return nullptr;  // Invalid
    };
    
    // Scan all segment cursors
    for (auto& cursor : _segment_cursors) {
        if (auto found = get_best(cursor.get())) {
            best = found;
        }
    }
    
    // Check base cursor (nullptr = use _base_cursor)
    if (auto found = get_best(&_base_cursor)) {
        best = nullptr;  // Base cursor wins
    }
    
    _active_cursor = best;
    
    // If we found a key, advance all other cursors past duplicates
    if (has_best) {
        Slice current_key = best ? best->key() : _base_cursor.key();
        
        for (auto& cursor : _segment_cursors) {
            if (cursor.get() != best && cursor->is_valid() &&
                cursor->key() == current_key) {
                cursor->next();
            }
        }
        
        if (best != nullptr && _base_cursor.is_valid() &&
            _base_cursor.key() == current_key) {
            _base_cursor.next();
        }
    }
}

// Helper: Find cursor with maximum key (for backward iteration)
// Uses get_best lambda that skips tombstones and tracks best key
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_find_max_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;
    
    // Lambda: move cursor backward past tombstones, return cursor if it has best key
    auto get_best = [&](auto* cursor) {
        while (cursor->is_valid()) {
            auto* leaf = cursor->leaf();
            if (!leaf || !leaf->is_tombstone()) {
                Slice key = cursor->key();
                if (!has_best || key > best_key) {
                    best_key = key;
                    has_best = true;
                    return cursor;
                }
                return nullptr;  // Not best
            }
            cursor->prev();  // Skip tombstone
        }
        return nullptr;  // Invalid
    };
    
    // Scan all segment cursors
    for (auto& cursor : _segment_cursors) {
        if (auto found = get_best(cursor.get())) {
            best = found;
        }
    }
    
    // Check base cursor (nullptr = use _base_cursor)
    if (auto found = get_best(&_base_cursor)) {
        best = nullptr;  // Base cursor wins
    }
    
    _active_cursor = best;
    
    // If we found a key, move all other cursors backward past duplicates
    if (has_best) {
        Slice current_key = best ? best->key() : _base_cursor.key();
        
        for (auto& cursor : _segment_cursors) {
            if (cursor.get() != best && cursor->is_valid() &&
                cursor->key() == current_key) {
                cursor->prev();
            }
        }
        
        if (best != nullptr && _base_cursor.is_valid() &&
            _base_cursor.key() == current_key) {
            _base_cursor.prev();
        }
    }
}
```

**Performance Analysis:**

- **N=1 (typical):** 2 comparisons (segment + base cursor) = O(1)
- **N=5 (max practical):** 6 comparisons = O(N) but faster than O(log N) heap
- **Memory:** No heap allocations, just stack variables
- **Code size:** ~200 lines smaller than priority queue approach

#### 2.4 Segment Reference Management and Validity Checking

**Strategy:** Cursors maintain references to segments. When background merge completes, references become obsolete and cursor repositions to persistent storage.

**Database tracks merge progress:**
```cpp
struct _LSMDB {
    std::atomic<tid_t> _last_merged_txn_id{0};
    
    void _on_merge_complete(tid_t txn_id) {
        _last_merged_txn_id.store(txn_id, std::memory_order_release);
    }
};
```

**Cursor snapshot acquisition** (only areas not yet merged):

```cpp
template <typename DB_, typename Traits_>
bool _LSMCursor<DB_, Traits_>::start_transaction(bool non_blocking) {
    // Call base cursor to start transaction
    bool result = _base_cursor.start_transaction(non_blocking);
    
    if (result) {
        // Add current transaction's segment at index 0 (for writes)
        _add_current_segment();
    }
    
    return result;
}

template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_acquire_segment_refs() {
    _release_segment_refs();
    
    if (!_base_cursor._txn) return;
    
    auto& cursor_txn = static_cast<typename DB::Transaction&>(*_base_cursor._txn);
    tid_t cursor_txn_id = cursor_txn.txn_id;
    
    // Collect segments from all COMMITTING transactions that are older than cursor's transaction
    // This ensures we see all committed data that was visible when cursor started
    _base_cursor._db->iter_transactions([&](txn_ptr txn) -> bool {
        auto& lsm_txn = static_cast<typename DB::Transaction&>(*txn);
        
        // Stop when we reach transactions newer than cursor's transaction
        if (lsm_txn.txn_id > cursor_txn_id) {
            return true;  // Break iteration
        }
        
        // Only collect segments from COMMITTING transactions
        if (lsm_txn.merge_phase == COMMITTING) {
            // Collect all segments from this transaction (oldest first in linked list)
            offset_t seg_offset = lsm_txn.segment_head;
            while (seg_offset) {
                Segment* segment = _base_cursor._db->resolve(seg_offset, READ);
                segment->acquire();
                
                // Create cursor for this segment
                // Segment derives from _MemoryDB, so cursor uses it directly as database
                auto cursor = std::make_unique<SegmentCursor>(segment);
                _segment_cursors.push_back(std::move(cursor));
                
                seg_offset = segment->next;
            }
        }
        
        return false;  // Continue to next transaction
    });
    
    // Reverse to get newest first (search priority: newest data checked first)
    std::reverse(_segment_cursors.begin(), _segment_cursors.end());
    
    _segment_refs_txn_id = cursor_txn_id;
}
```

**Segment reference validity checking** (at start of every find/move operation):
```cpp
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_check_segment_refs_validity() {
    // Only check if cursor has segment references
    if (_segment_cursors.empty()) return;
    
    auto& cursor_txn = static_cast<typename DB::Transaction&>(*_base_cursor._txn);
    
    // If cursor's transaction is COMMITTED, all its segments are merged - release them
    if (cursor_txn.merge_phase == COMMITTED) {
        std::string saved_key;
        if (is_valid()) {
            saved_key = key().to_string();
        }
        _release_segment_refs();
        _base_cursor.stack.clear();
        if (!saved_key.empty()) {
            find(saved_key);  // Reposition to persistent storage
        }
    }
}
```

**Segment reference cleanup:**
```cpp
template <typename DB_, typename Traits_>
void _LSMCursor<DB_, Traits_>::_release_segment_refs() {
    if (_segment_cursors.empty()) return;
    
    // Release all segment cursors
    for (auto& cursor : _segment_cursors) {
        // Cursor's _db points to the Segment
        Segment* segment = static_cast<Segment*>(cursor->_db.get());
        segment->release();
    }
    _segment_cursors.clear();
    _active_cursor = nullptr;
}
```

**Performance characteristics:**
- **No segment refs:** Single empty check (branch predictor friendly)
- **With segment refs:** One atomic load per operation
- **Reposition:** Happens exactly once when merge completes
- **Minimal overhead:** Most cursors have no segment references after initial merge


### Phase 3: Background Thread (2 weeks)

**Background Thread Responsibilities:**
1. **Merge segments** to persistent storage (oldest first from transaction queue)
2. **Prune tombstones** in separate cycle when no merge operations waiting
3. **Trigger auto-commit** when timeout expires after last write

**Note:** Tombstone pruning is scheduled as a lower-priority task that can be interrupted when merge operations arrive. Cursors handle tombstones as non-existing values during find/move operations.

#### 3.1 Background Thread Infrastructure

**File:** `include/leaves/intern/_lsm_db.hpp`

Implement background thread methods in `_LSMDB`:

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::_start_background_thread() {
    _background_running = true;
    _last_write_time = std::chrono::steady_clock::now();
    _background_thread = std::thread([this]() { _background_loop(); });
}

template <typename Storage_>
void _LSMDB<Storage_>::_stop_background_thread() {
    _background_running = false;
    _background_cv.notify_all();
    if (_background_thread.joinable()) {
        _background_thread.join();
    }
}
```

#### 3.2 Background Loop

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::_background_loop() {
    auto* header = _get_lsm_header();
    
    while (header->background_running.load(std::memory_order_acquire)) {
        // Collect all transactions that can be merged in this round
        std::vector<txn_ptr> txns_to_merge;
        bool any_sync_commit = false;
        
        // Walk transaction chain from oldest to newest using iter_transactions
        // Lambda returns true to break iteration, false to continue
        this->iter_transactions([&](txn_ptr txn) -> bool {
            auto& lsm_txn = static_cast<Transaction&>(*txn);
            
            if (lsm_txn.merge_phase == COMMITTED) {
                // Already committed, continue to next
                return false;
            } else if (lsm_txn.merge_phase == COMMITTING) {
                // Ready for merge - add to batch
                txns_to_merge.push_back(txn);
                if (lsm_txn.sync_commit) {
                    any_sync_commit = true;
                }
                return false;  // Continue to next transaction
            } else if (lsm_txn.merge_phase == WRITING) {
                // Transaction still being written - stop here
                // Cannot merge transactions beyond this point
                return true;  // Break iteration
            }
            return false;
        });
        
        if (!txns_to_merge.empty()) {
            // Merge all transactions in this round
            for (auto& txn : txns_to_merge) {
                _merge_transaction(txn);
            }
            
            // Set all transactions to COMMITTED state
            tid_t max_merged_txn_id = 0;
            for (auto& txn : txns_to_merge) {
                auto& lsm_txn = static_cast<Transaction&>(*txn);
                lsm_txn.merge_phase = COMMITTED;
                if (lsm_txn.txn_id > max_merged_txn_id) {
                    max_merged_txn_id = lsm_txn.txn_id;
                }
            }
            
            // Perform single synchronized flush if any transaction requires it
            // This flushes the COMMITTED state to disk along with merged data
            if (any_sync_commit) {
                this->sync();
            }
            
            // Now release refs - segments can be freed after sync completes
            for (auto& txn : txns_to_merge) {
                auto& lsm_txn = static_cast<Transaction&>(*txn);
                lsm_txn.refs.fetch_sub(1, std::memory_order_release);
            }
            
            // Update last merged transaction ID for cursor validation
            header->last_merged_txn_id.store(max_merged_txn_id, std::memory_order_release);
        } else {
            // No merge work, wait for notification or timeout
            std::unique_lock<boost::interprocess::interprocess_mutex> lock(header->background_mutex);
            header->background_cv.wait_for(lock, 
                boost::posix_time::milliseconds(50),
                [&]() { return !header->background_running.load(std::memory_order_acquire); });
        }
    }
}
```

#### 3.3 Transaction Merge and Tombstone Pruning

**File:** `include/leaves/intern/_lsm_db.hpp`

**Transaction Merge** - Merge all segments from a committed transaction:

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::_merge_transaction(txn_ptr& txn) {
    auto& lsm_txn = static_cast<Transaction&>(*txn);
    
    // Iterate through segments (oldest first from segment_head)
    offset_t seg_offset = lsm_txn.segment_head;
    while (seg_offset) {
        Segment* segment = this->resolve(seg_offset, WRITE);
        
        // Create destination cursor (persistent storage)
        Cursor dst_cursor(this);
        
        // Create source cursor for segment's trie
        Cursor src_cursor(this);
        // TODO: Configure src_cursor to use segment's trie_root
        
        // Merge using _Merger from _merger.hpp
        _Merger<Cursor, Cursor> merger(dst_cursor, src_cursor);
        merger.exec();
        
        // Move to next segment
        // Note: Segment state tracked by transaction's merge_phase
        offset_t next_seg = segment->next;
        
        // Segment will auto-recycle when all cursor references are released
        seg_offset = next_seg;
    }
    
    // Note: COMMITTED state, sync, and refs release are handled by _background_loop
    // after all transactions in the round are merged
}
```

**Tombstone Pruning** - Separate cycle for cleaning tombstones (interruptible):

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::_prune_tombstones_cycle() {
    auto* header = _get_lsm_header();
    
    // Check if merge work arrived (check for COMMITTING transactions)
    for (auto txn : this->iter_transactions()) {
        auto& lsm_txn = static_cast<Transaction&>(*txn);
        if (lsm_txn.merge_phase == COMMITTING) {
            return;  // Interrupt pruning, merge has priority
        }
    }
    
    // Scan persistent storage for tombstones and remove them
    Cursor cursor(this);
    cursor.first();
    
    while (cursor.is_valid()) {
        // Check for interruption periodically
        bool has_merge_work = false;
        for (auto txn : this->iter_transactions()) {
            auto& lsm_txn = static_cast<Transaction&>(*txn);
            if (lsm_txn.merge_phase == COMMITTING) {
                has_merge_work = true;
                break;
            }
        }
        if (has_merge_work) {
            cursor.commit();  // Commit any removes done so far before interrupting
            return;  // Interrupt pruning
        }
        
        // Check if current entry is tombstone
        auto* leaf = cursor.leaf();
        if (leaf && leaf->is_tombstone()) {
            cursor.remove();
        } else {
            cursor.next();
        }
    }
    
    // Commit all remove operations at the end
    cursor.commit();
}
```

**Note:** Cursors already handle tombstones as non-existing values in find/next/prev operations, so tombstone pruning is a cleanup optimization, not required for correctness.

#### 3.4 Database Initialization and Crash Recovery

**File:** `include/leaves/intern/_lsm_db.hpp`

Add to `_LSMDB` initialization:

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::open(const char* path) {
    // Call base class open
    BaseDB::open(path);
    
    // Sanitize database state after crash or unclean shutdown
    _sanitize_database();
    
    // Start background thread
    _start_background_thread();
}

template <typename Storage_>
void _LSMDB<Storage_>::_sanitize_database() {
    // Clean up inconsistent state left by crash or unclean shutdown
    // Note: WRITING transactions are never persistent (not committed),
    // so they never appear in the transaction chain after a crash
    
    // Walk all transactions and sanitize their state
    this->iter_transactions([&](txn_ptr txn) -> bool {
        auto& lsm_txn = static_cast<Transaction&>(*txn);
        
        if (lsm_txn.merge_phase == COMMITTING) {
            // Transaction was committed but merge may not have completed
            // Merge it now (no cursors exist yet at startup)
            _merge_transaction(txn);
            lsm_txn.merge_phase = COMMITTED;
        }
        // COMMITTED transactions are fine - already merged
        
        // Reset all segment refs to 0 for recycling eligibility
        SegmentResolver<_LSMDB> resolver(this);
        lsm_txn._free_segments.iter(resolver, [&](auto& block_item) {
            auto segment = this->template resolve<SegmentType>(block_item.link, WRITE);
            segment->refs.store(0, std::memory_order_relaxed);
        });
        
        return false;  // Continue to next transaction
    });
    
    BaseDB::sanitize();
}
```

**Sanitization Logic:**

1. **WRITING transactions** (active during crash):
   - **Never appear in transaction chain** - transactions are only added to chain on commit
   - No cleanup needed - they were never persisted
   - Rationale: Uncommitted work doesn't survive crash

2. **COMMITTING transactions** (commit done, merge incomplete):
   - Merge immediately at startup
   - Set merge_phase to COMMITTED after merge
   - Rationale: Resume merge where it left off

3. **COMMITTED transactions**:
   - No action needed
   - Already fully merged

4. **Segment refcount reset**:
   - All refs set to 0 via iter() on _free_segments
   - Rationale: Cursors don't survive process restart
   - Segments can be recycled immediately
   - Note: Segment state is tracked by transaction's merge_phase, no separate sync needed

**Why this matters:**
- **Maintains ACID semantics** - uncommitted transactions don't survive crashes
- Cleans up stale refcounts that would prevent recycling
- Ensures consistent state for background thread
- Handles all crash scenarios gracefully
```

### Phase 4: Multi-Writer Database (1 week)

#### 4.1 Multi-Writer Architecture

**Key Design Principle:** Multi-writer support requires each concurrent transaction to have its own segment pool. Since structures can be memory-mapped, we cannot use `std::vector` or `std::mutex` - must use offset-based structures and `boost::interprocess::interprocess_mutex`.

**File:** `include/leaves/intern/_lsm_db.hpp`

**Segment Pool Structure** (memory-mapped compatible):

```cpp
// Fixed-size segment pool for multi-writer mode
template <typename Traits_>
struct SegmentPool {
    static constexpr uint16_t MAX_WRITERS = 64;
    
    boost::interprocess::interprocess_mutex pool_mutex;
    uint16_t pool_size{0};
    offset_t slots[MAX_WRITERS];        // Offsets to _GarbageSlot instances
    uint8_t slot_in_use[MAX_WRITERS];  // 1 = allocated, 0 = free
};
```

**Multi-Writer Database:**

```cpp
template <typename Storage_>
struct _MultiWriterDB : public _LSMDB<Storage_> {
    offset_t _segment_pool_offset{0};  // Pool in database header
    
    void start_transaction() {
        BaseDB::start_transaction();
        // Allocate slot from pool, store in lsm_txn._pool_slot_index
    }
    
    bool commit(uint64_t cursor_id = 0, bool sync = false) {
        // Free pool slot after commit
    }
};
```

**Transaction Extension:**

```cpp
struct _LSMTransaction {
    int16_t _pool_slot_index{-1};  // Pool slot (-1 = not using pool)
    _GarbageSlot<Traits_> _free_segments;  // Transaction-local
};
```

### Phase 5: _MultiWriterCursor Implementation (2 weeks)

#### 5.1 Multi-Writer Transaction Management

**Key Design:** Multiple concurrent transactions can be active, but merges must occur in transaction ID order.

```cpp
// Transaction lifecycle for MultiWriter:
// 1. start_transaction() - allocates segment, adds to merge queue with WRITING phase
// 2. Write operations - modify current transaction's segments
// 3. commit() - changes phase to COMMITTING, signals background thread
// 4. Background thread - merges transactions in txn_id order

// Why ID ordering matters:
// - Ensures serializable merge order even with concurrent commits
// - Later transactions may depend on earlier ones being merged first
// - Prevents merge conflicts from out-of-order application

// Example timeline:
// T1 (id=100): start -> write -> commit (COMMITTING)
// T2 (id=101): start -> write -> commit (COMMITTING)  
// T3 (id=102): start -> write -> still WRITING
//
// Merge order: T1, then T2, then wait for T3 to commit
// Even if T2 commits before T1, T1 merges first
```

#### 5.2 Multi-Writer Cursor Structure

**File:** `include/leaves/intern/_cursor.hpp`

```cpp
// Handler for multi-writer conflict resolution
// Similar to merge handler but for concurrent writes
struct MultiWriterHandler {
    std::function<bool(const Slice& key, 
                       const Slice& my_value,
                       const Slice& their_value)> callback;
    
    // Called by merge logic when conflict detected
    template <typename DstTransition, typename SrcTransition>
    bool overwrite(const std::string& key,
                   DstTransition& dst,
                   SrcTransition& src) {
        if (!callback) return true;  // Default: last write wins
        
        auto my_value = src.value();
        auto their_value = dst.value();
        return callback(key, my_value, their_value);
    }
};

// Multi-writer cursor - NOT transactional
template <typename DB_, typename Traits_>
struct _MultiWriterCursor : public _LSMCursor<DB_, Traits_> {
    typedef _LSMCursor<DB, Traits_> LSMCursor;
    
    MultiWriterHandler _handler;
    
    _MultiWriterCursor(typename Traits::db_ptr db) : LSMCursor(db) {}
    
    // Disable transaction methods
    bool start_transaction(bool non_blocking = false) = delete;
    bool commit(bool sync = false) = delete;
    bool rollback() = delete;
    
    // Set conflict resolution callback
    // Note: std::function may heap-allocate if callback captures exceed ~16-32 bytes (SBO limit)
    // Once constructed, calling the function does NOT allocate - only construction/copy/assignment
    void set_conflict_callback(std::function<bool(
        const Slice& key,
        const Slice& my_value,
        const Slice& their_value)> callback) {
        _handler.callback = callback;
    }
    
    // Write operations (conflict detection)
    void value(const Slice& value);
    void* reserve(size_t size);
    void remove();  // Inherited from LSMCursor - inserts tombstone
    
    // Private methods (prefixed with _)
    bool _check_conflict(const Slice& key, const Slice& new_value);
};
```

#### 5.3 Conflict Resolution

Multi-writer conflicts are resolved during the merge phase using the handler pattern:

```cpp
template <typename DB_, typename Traits_>
void _MultiWriterCursor<DB_, Traits_>::value(const Slice& value) {
    // Write to MemoryDB
    LSMCursor::value(value);
    
    // Conflicts will be detected and resolved during background merge
    // when _Merger calls handler.overwrite() for overlapping keys
}

// The handler is used by _Merger during merge:
template <typename Storage_>
void _LSMDB<Storage_>::_merge_multi_writer_memdbs(MemoryDBArea* area1, 
                                                    MemoryDBArea* area2) {
    // Create cursors for both MemoryDB areas
    _NodeIterator<_MemoryDB, MemoryTraits> dst_iter(&_mem_storage, area1->memdb_root);
    _NodeIterator<_MemoryDB, MemoryTraits> src_iter(&_mem_storage, area2->memdb_root);
    
    // Use multi-writer handler for conflict resolution
    MultiWriterHandler handler = /* get from cursor or database config */;
    
    _Merger<decltype(dst_iter), decltype(src_iter), MultiWriterHandler> merger(
        dst_iter, src_iter, handler
    );
    
    merger.exec();
}
```

### Phase 6: Optimization & Testing (1-2 weeks)

#### 6.1 Memory Pressure Handling

```cpp
template <typename Storage_>
void _LSMDB<Storage_>::_check_memory_pressure() {
    constexpr uint64_t MAX_MEMDB_MEMORY = 64 * 1024 * 1024;  // 64MB
    
    if (_active_txn->total_memdb_bytes > MAX_MEMDB_MEMORY) {
        // Force commit and merge
        commit(_active_txn->cursor_id, false);
        
        // Wait for merge to complete if still over limit
        while (_get_total_memdb_bytes() > MAX_MEMDB_MEMORY / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
```

#### 6.2 Benchmarking

Create benchmark suite testing:
- Single-writer with `_Cursor` (should maintain 577 MB/s)
- Single-writer with `_LSMCursor` (target 900 MB/s)
- Multi-writer with `_MultiWriterCursor`
- Read performance with queued segments
- Auto-commit timing accuracy

#### 6.3 Testing Checklist

- [ ] Unit tests for Segment-level refcounting
- [ ] Unit tests for area recycling via _GarbageSlot
- [ ] Unit tests for tombstone flag operations
- [ ] Unit tests for multi-source search order
- [ ] Integration test: write → commit → verify merge
- [ ] Integration test: cursor navigation with tombstones
- [ ] Integration test: auto-commit timeout
- [ ] Integration test: multi-writer conflicts
- [ ] Crash recovery test: interrupt during WRITING phase
- [ ] Crash recovery test: interrupt during MERGING phase
- [ ] Performance regression test for `_Cursor`
- [ ] Memory leak test with area cleanup
- [ ] Stress test: many concurrent writers

## Usage Guidelines

### When to Use Each Cursor Type

**`_Cursor`** - Use when:
- Single-writer application
- Need full ACID transactions
- Maximum single-threaded write performance
- Simple use case, no need for LSM features

**`_LSMCursor`** - Use when:
- Burst writes with periods of inactivity
- Need transactional guarantees with LSM performance
- Read-heavy workload benefits from in-memory buffering
- Want automatic background merging and tombstone pruning

**`_MultiWriterCursor`** - Use when:
- Multiple concurrent writers needed
- Can tolerate eventual consistency
- Have application-level conflict resolution logic
- Need maximum multi-writer throughput

### Configuration Options

```cpp
// Note: Segment size is fixed at 64KB
// Configure max total memory before forced merge
lsm_db.set_max_memory(128 * 1024 * 1024);  // 128MB

// Enable auto-commit (database-level)
lsm_db.enable_auto_commit(500);  // 500ms timeout after last write

// Set conflict callback
multi_cursor.set_overwrite_callback(
    [](const Slice& key, const Slice& mine, const Slice& theirs) -> Slice {
        // Last write wins
        return mine;
    }
);
```

## Performance Expectations

### Write Performance
- `_Cursor`: 577 MB/s sequential, 291 MB/s random (baseline)
- `_LSMCursor`: ~900 MB/s sequential, ~450 MB/s random (target)
- `_MultiWriterCursor`: Scales with writer count

### Read Performance
- Should maintain: 312 MB/s random, 2516 MB/s sequential
- Slight overhead for multi-source search in `_LSMCursor`
- Cache-friendly area layout helps mitigate search cost

### Memory Usage
- 64KB per segment (fixed size - optimized for cache efficiency and low overhead)
- Max 64MB total before forced merge (configurable, ~1000 segments)
- Segment overhead: ~90 bytes per segment (header + MemManager = 0.14% overhead)
- Usable space: ~64KB-65KB per segment (99%+ efficiency)
- Recycling pool: Segments returned via `_GarbageSlot`, one 2MB area yields ~32 segments
- Area allocation: Only when segment pool exhausted
- Typical 10MB transaction: ~160 segments (manageable linked list traversal)

## Future Enhancements

1. **Compaction**: Merge multiple small MemoryDBs before persisting
2. **Bloom Filters**: Speed up negative lookups in MemoryDBs
3. **Level-based LSM**: Multiple levels with size-tiered compaction
4. **Write-ahead Log**: Durability for MemoryDB contents
5. **Adaptive Thresholds**: Adjust MemoryDB size based on workload

## Implementation Readiness Assessment

### ✅ Fully Specified

1. **Segment Structure** - Derives from `_MemoryDB`, provides database interface for cursors
2. **Overflow Detection** - `alloc_single_area()` returns 0 when segment full, `_Inserter` checks nullptr
3. **Transaction Initialization** - `start_transaction()` allocates segment, adds to merge queue immediately
4. **Rollback Mechanism** - `_GarbageSlot` state restoration automatically handles segment cleanup
5. **Cursor-Segment Integration** - Cursors initialized with Segment pointer (Segment is _MemoryDB)
6. **Write Path** - `_Inserter` must handle nullptr from `alloc()`, triggers `_handle_segment_overflow()`
7. **Navigation** - Heap queue with reversed ordering for `prev()` vs `next()`
8. **Crash Recovery** - Walk transaction chain, check segment merge states
9. **Auto-commit** - Background thread checks: active transaction, empty queue, elapsed time
10. **Segment Initialization** - `init()` called in `start_transaction()` and after overflow
11. **Segment Recycling** - Immediately push to `_GarbageSlot`, `may_recycle()` checks refcount==0 and merge_state==COMMITTED
12. **Transaction Queue** - Added on creation (WRITING), merged only after commit (COMMITTING), ordered by txn_id
13. **MultiWriter Ordering** - Transactions merge in txn_id order even with concurrent commits

### ⚠️ Implementation Details Needed

1. **SegmentTraits Definition** - Complete trait specialization with 512-byte BlockContainer
2. **Exception vs nullptr** - Decision needed: exception handling vs nullptr return for overflow
3. **Big Memory Forwarding** - Interface between Segment and LSMDB for values > threshold
4. **Transaction Time Tracking** - `get_transaction_last_modified()` method in base DB
5. **Transaction Enumeration** - `get_all_transactions()` for crash recovery
6. **Segment Mutex Location** - Added to `_LSMTransaction`, needs integration with release()
7. **_check_segment_refs_validity()** - Update to use `_segment_cursors.empty()` not `_segment_refs.empty()`
8. **SegmentOverflowException** - Define exception class or use nullptr pattern consistently

### 📝 Minor Documentation Gaps

1. Exact signature of `alloc_big_memory()` and `free_big_memory()` forwarding
2. How `_Merger` template parameters work with Segment cursors
3. Memory layout details for 512-byte BlockContainer trait override
4. Thread-safety of segment release with per-transaction mutex

### ✅ Ready to Implement

**Yes, with clarifications above.** The document now provides:

- Clear architectural decisions (Segment derives from _MemoryDB)
- Complete data flows (overflow detection → new segment allocation)
- Specific integration points (`cursor->_db` is Segment, not LSMDB)
- Implementation strategies (heap queues, transaction queue, crash recovery walk)

**Remaining decision:** Exception vs nullptr for overflow? Recommendation: **nullptr** is faster (no stack unwinding) and simpler for hot path.

## References

- Original proposal: `readme.md` lines 134-156
- Benchmark data: `readme.md` lines 1-18
- Current implementation: `include/leaves/intern/_cursor.hpp`
- Merger algorithm: `include/leaves/intern/_merger.hpp`
- Transaction structure: `include/leaves/intern/_db.hpp`


## Corrections
The current LSM architecture has a major flaw:
Using the LSMDB Base Class transaction for cursors and background threads leads to a dead lock.

So the archicture shall be changed:
- The base storage (the last lsm level) shall be a contained _DB instance (not the base class of LSMDB as it is right now)
- The LSMDB shall be a facade for Segment and the contained _DB instance and implement its onw transaction managment
- For now just mock the bigvalue handling
