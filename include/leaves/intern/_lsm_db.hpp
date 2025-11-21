#ifndef _LEAVES__LSM_DB_HPP
#define _LEAVES__LSM_DB_HPP

#include "_db.hpp"
#include "_memory.hpp"
#include "_memstore.hpp"
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <thread>
#include <atomic>

namespace leaves {

// Forward declarations
template <typename LSMDB_>
struct Segment;

template <typename LSMDB_>
struct SegmentResolver;

// Merge phase states for LSM transactions
enum MergePhase : uint8_t {
    WRITING = 0,      // Active transaction, writes going to segment
    COMMITTING = 1,   // Committed, ready for merge
    COMMITTED = 2     // Merge complete, transaction fully committed
};

// Segment-specific traits with smaller BlockContainer
template <typename BaseTraits_>
struct SegmentTraits : public BaseTraits_ {
    typedef BaseTraits_ BaseTraits;
    
    // Smaller BlockContainer for segment's limited space (512 bytes instead of 4KB)
    static constexpr size_t BLOCK_CONTAINER_SIZE = 512;
};

// LSM Header - extends base DB header with multi-process LSM state
template <typename Storage_>
struct LSMHeader : public _DBHeader<Storage_> {
    using BaseHeader = _DBHeader<Storage_>;
    using Traits = typename Storage_::Traits;
    
    // Last merged transaction ID (for cursor validation)
    std::atomic<tid_t> last_merged_txn_id{0};
    
    // Background thread coordination (multi-process safe)
    boost::interprocess::interprocess_mutex background_mutex;
    boost::interprocess::interprocess_condition background_cv;
    
    // Auto-commit configuration (shared across processes)
    uint32_t auto_commit_ms{200};
    uint8_t auto_commit_enabled{0};
    
    void init() {
        last_merged_txn_id.store(tid_t(0), std::memory_order_release);
        auto_commit_ms = 200;
        auto_commit_enabled = 0;
    }
};

// LSM-specific transaction - extends base transaction
template <typename Traits_>
struct _LSMTransaction : public _Transaction<Traits_> {
    typedef _Transaction<Traits_> BaseTransaction;
    using Traits = Traits_;
    using ptr = typename Traits::Pointer<_LSMTransaction>;
    using offset_e = typename Traits_::offset_e;
    
    // LSM-specific fields
    offset_e segment_head{0};            // Head of segment linked list (oldest first)
    offset_e current_segment{0};         // Current segment offset being written
    uint8_t merge_phase{WRITING};
    uint8_t sync_commit{0};              // 1 = committed with sync, requires sync flush after merge
    
    // Segment recycling via _GarbageSlot (fixed 64KB segments)
    _GarbageSlot<Traits_> _free_segments;
    
    static constexpr uint32_t SEGMENT_SIZE = 65536; // 64KB per segment
    
    // Override size to include LSM fields
    uint16_t size() const { 
        return sizeof(_LSMTransaction); 
    }
    
    // Allocate a new segment for this transaction
    template <typename DB>
    offset_e _allocate_segment(DB* db) {
        // Create segment resolver for this transaction
        SegmentResolver<DB> resolver(db, this);
        
        // Try to pop from recycle pool (transaction-safe)
        auto segment_block_ptr = _free_segments.pop(resolver);
        if (segment_block_ptr) {
            // Reuse existing segment from pool (already initialized)
            offset_e segment_offset = db->resolve(segment_block_ptr);
            auto segment_ptr = db->template resolve<typename DB::SegmentType>(segment_offset, WRITE);
            
            // Reset segment state
            segment_ptr->next = 0;
            segment_ptr->cursor_refs.store(0, std::memory_order_release);
            segment_ptr->reinit(db);
            
            // Immediately push back to _GarbageSlot for future recycling
            // SegmentResolver::may_recycle() will check cursor_refs and merge_phase when popping
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
            auto seg_ptr = db->template resolve<typename DB::SegmentType>(seg_offset, WRITE);
            
            // Calculate data bounds for this segment
            // data_start accounts for Segment header (includes BaseMemoryDB fields + LSM fields)
            offset_t data_start = seg_offset + sizeof(typename DB::SegmentType);
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
        offset_e new_segment_offset = db->resolve(new_segment_block_ptr);
        auto new_segment_ptr = db->template resolve<typename DB::SegmentType>(new_segment_offset, WRITE);
        new_segment_ptr->reinit(db);
        return new_segment_offset;
    }
};

// Segment represents a fixed-size (64KB) storage block for one transaction's writes
// Segments contain a MemManager for memory allocation within the segment
template <typename LSMDB_>
struct Segment {
    typedef LSMDB_ LSMDB;
    typedef typename LSMDB::Traits Traits;
    typedef _MemManager<Traits> MemManager;
    using offset_e = typename Traits::offset_e;
    
    MemManager _mem_manager;
    offset_t allocation_start{0};
    offset_t next{0};                // Next segment in the linked list (offset, not pointer)
    std::atomic<int> cursor_refs{0}; // Refcount tracks all cursors using this segment
    LSMDB* parent_db{nullptr};       // Parent LSMDB for forwarding big memory operations
    
    // Initialize segment's memory manager with specific bounds
    // Called once when area is split into segments
    void init(offset_t data_start, offset_t data_end) {
        allocation_start = data_start;
        _mem_manager.init(allocation_start, data_end);
        
        next = 0;
        cursor_refs.store(0, std::memory_order_release);
    }
    
    // Set parent DB pointer before returning segment to user
    void reinit(LSMDB* parent) {
        _mem_manager.allocation_start = allocation_start;
        parent_db = parent;
    }
    
    // Allocate memory within this segment
    template <typename Resolver>
    auto alloc(uint16_t slot_id, Resolver& resolver) {
        return _mem_manager.alloc(slot_id, resolver);
    }
    
    // Cursor management
    void acquire() { 
        cursor_refs.fetch_add(1, std::memory_order_relaxed); 
    }
    
    template <typename Transaction>
    void release(_GarbageSlot<Traits>& free_segments, Transaction* txn) {
        if (cursor_refs.fetch_sub(1, std::memory_order_release) == 1) {
            // Last reference released, check if can recycle
            if (txn && txn->merge_phase == COMMITTED) {
                // Push to garbage slot (transaction-safe, no lock needed)
                free_segments.push(this->offset());
            }
        }
    }
    
    template <typename Transaction>
    bool can_recycle(Transaction* txn) const { 
        assert(txn);
        return cursor_refs.load(std::memory_order_acquire) == 0 && 
               txn->merge_phase == COMMITTED; 
    }
};

// Custom resolver for segment recycling
template <typename LSMDB_>
struct SegmentResolver {
    typedef LSMDB_ LSMDB;
    typedef typename LSMDB::SegmentType Segment;
    typedef typename LSMDB::Traits Traits;
    typedef typename Traits::offset_e offset_e;
    typedef typename Traits::template Pointer<Segment> block_ptr;
    typedef _BlockContainer<Traits> BlockContainer;
    typedef typename BlockContainer::ptr cont_ptr;
    typedef typename LSMDB::Transaction Transaction;
    
    LSMDB* db;
    Transaction* txn;
    
    SegmentResolver(LSMDB* db_, Transaction* txn_) : db(db_), txn(txn_) {}
    
    // Resolve BlockItem.link offset to Segment pointer
    block_ptr resolve(offset_e offset, Access mode) {
        return db->template resolve<Segment>(offset, mode);
    }
    
    // Resolve various pointer types to offset (used by _GarbageSlot)
    typedef typename Traits::ptr base_ptr;
    
    offset_e resolve(const base_ptr& ptr) {
        return db->resolve(ptr);
    }
    
    offset_e resolve(const cont_ptr& ptr) {
        return db->resolve(ptr);
    }
    
    offset_e resolve(const block_ptr& ptr) {
        return db->resolve(ptr);
    }
    
    // Check if segment can be recycled (cursor_refs == 0 and merge complete)
    template <typename BlockItem>
    bool may_recycle(const BlockItem& item) {
        auto segment = db->template resolve<Segment>(item.link, READ);
        return segment->can_recycle(txn);
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

// LSM Database - derives from base DB
template <typename Storage_>
struct _LSMDB : public _DB<Storage_, _LSMTransaction<typename Storage_::Traits>, LSMHeader<Storage_>> {
    typedef _DB<Storage_, _LSMTransaction<typename Storage_::Traits>, LSMHeader<Storage_>> BaseDB;
    typedef _LSMTransaction<typename Storage_::Traits> Transaction;
    typedef Segment<_LSMDB<Storage_>> SegmentType;
    using txn_ptr = typename BaseDB::txn_ptr;
    using Traits = typename Storage_::Traits;
    using offset_e = typename Traits::offset_e;
    using block_ptr = typename Traits::ptr;
    using Header = LSMHeader<Storage_>;
    
    // Background thread (process-local)
    std::thread _background_thread;
    std::atomic<bool> _shutdown{false};
    
    _LSMDB(Storage_& storage, offset_t header, uint16_t index) 
        : BaseDB(storage, header, index) {
        _start_background_thread();
    }
    
    _LSMDB(Storage_& storage, offset_t* header, uint16_t index) 
        : BaseDB(storage, header, index) {
        // Initialize LSM header for new database
        this->_header->init();
        _start_background_thread();
    }

    ~_LSMDB() {
        _stop_background_thread();
    }
    
    // Auto-commit API (configuration stored in LSMHeader)
    void enable_auto_commit(uint32_t timeout_ms = 200) {
        this->_header->auto_commit_enabled = 1;
        this->_header->auto_commit_ms = timeout_ms;
    }
    
    void disable_auto_commit() {
        this->_header->auto_commit_enabled = 0;
    }
    
    // Background thread operations
    void _start_background_thread();
    void _stop_background_thread();
    void _background_loop();
    
    // LSM operations performed by background thread
    void _merge_transaction(txn_ptr& txn);
    void _prune_tombstones_cycle();
    void _sanitize_database();
    
    // Override start_transaction to allocate initial segment
    txn_ptr start_transaction(uint64_t cursor_id, bool nonblocking = false) {
        txn_ptr result = BaseDB::start_transaction(cursor_id, nonblocking);
        
        // Allocate first segment (already initialized and has parent_db set)
        this->_wtxn->current_segment = this->_wtxn->_allocate_segment(this);
        this->_wtxn->segment_head = this->_wtxn->current_segment;
        this->_wtxn->merge_phase = WRITING;
        
        return result;
    }
    
    // Override rollback - _GarbageSlot rollback handles segment cleanup
    void rollback(uint64_t cursor_id) {
        // Transaction rollback automatically restores _free_segments to previous state
        // All segments allocated during transaction are returned to pool
        BaseDB::rollback(cursor_id);
    }
    
    // Override prepare_commit to flush segments in sync mode
    tid_t prepare_commit(uint64_t cursor_id, bool sync = false) {
        // Call base prepare_commit to update prepared_txn and next_txn
        tid_t result = BaseDB::prepare_commit(cursor_id, false);

        // In sync mode, flush segments before preparing transaction
        // This ensures segment data is durable before the transaction becomes visible
        if (result && sync) {
            // Flush all segments in this transaction
            offset_e segment_offset = this->_wtxn->segment_head;
            while (segment_offset) {
                auto segment = this->template resolve<SegmentType>(segment_offset, WRITE);
                
                // Flush only the used portion (allocation_start to allocation_end)
                offset_t start = segment->allocation_start;
                offset_t end = segment->_mem_manager.allocation_start;  // the last allocated position
                size_t used_size = end - start;
                
                this->flush(&*segment, start, used_size, true);  // sync=true
                segment_offset = segment->next;
            }
            
            // Flush prepared_txn pointer, read_txn->next_txn, and the whole _wtxn
            offset_t offset = this->_storage.resolve(block_ptr(&this->_header->prepared_txn));
            this->flush(&this->_header->prepared_txn, offset, sizeof(offset_t), true);  // prepared_txn field

            txn_ptr read_txn = this->template resolve<Transaction>(this->_header->read_txn);
            this->flush(&*read_txn, this->_header->read_txn, sizeof(Transaction), true);  // next_txn field
            
            this->flush(&*this->_wtxn, 0, this->_wtxn->size(), true);  // whole transaction
        }
        
        return result;
    }
    
    // Override commit to mark transaction as ready for merge
    void commit(bool sync = false) {
        // Increment refs to prevent transaction from being freed during merge
        this->_wtxn->refs.fetch_add(1, std::memory_order_release);
        
        // Change phase to COMMITTING - background thread will process it
        this->_wtxn->merge_phase = COMMITTING;
        
        // Record if this was a sync commit for later flush decision
        this->_wtxn->sync_commit = sync ? 1 : 0;
        
        // Notify background thread that a transaction is ready
        this->_header->background_cv.notify_one();
        
        // Commit transaction metadata (always with sync=false)
        // In sync mode, prepare_commit already flushed everything
        BaseDB::commit(false);

        if (sync) {
            // Flush read_txn (last committed transaction)
            auto offset = this->_storage.resolve(block_ptr(&this->_header->read_txn));
            this->flush(&this->_header->read_txn, offset, sizeof(offset_t), true);
        }
    }
};

// Background thread implementation
template <typename Storage_>
void _LSMDB<Storage_>::_start_background_thread() {
    _shutdown.store(false, std::memory_order_release);
    _background_thread = std::thread([this]() { _background_loop(); });
}

template <typename Storage_>
void _LSMDB<Storage_>::_stop_background_thread() {
    _shutdown.store(true, std::memory_order_release);
    this->_header->background_cv.notify_all();
    if (_background_thread.joinable()) {
        _background_thread.join();
    }
}

template <typename Storage_>
void _LSMDB<Storage_>::_background_loop() {
    while (!_shutdown.load(std::memory_order_acquire)) {
        bool any_sync_commit = false;
        tid_t max_merged_txn_id{0};
        bool merged_any = false;
        
        // Walk transaction chain from oldest to newest using iter_transactions
        this->iter_transactions([&](txn_ptr txn) -> bool {
            if (txn->merge_phase == COMMITTED) {
                // Already committed, continue to next
                return false;
            } else if (txn->merge_phase == COMMITTING) {
                // Ready for merge - merge it now
                if (txn->sync_commit) {
                    any_sync_commit = true;
                }
                
                _merge_transaction(txn);
                
                // Mark as committed
                txn->merge_phase = COMMITTED;
                if (txn->txn_id > max_merged_txn_id) {
                    max_merged_txn_id = txn->txn_id;
                }
                
                merged_any = true;
                return false;  // Continue to next transaction
            } else if (txn->merge_phase == WRITING) {
                // Transaction still being written - stop here
                // Cannot merge transactions beyond this point
                return true;  // Break iteration
            }
            return false;
        });
        
        if (merged_any) {
            // Perform single synchronized flush if any transaction requires it
            if (any_sync_commit) {
                this->_storage.flush(true);
            }
            
            // Now release refs on merged transactions - segments can be freed after sync completes
            this->iter_transactions([&](txn_ptr txn) -> bool {
                if (txn->txn_id <= max_merged_txn_id) {
                    assert(txn->merge_phase == COMMITTED);
                    txn->refs.fetch_sub(1, std::memory_order_release);
                }
                return false;
            });
            
            // Update last merged transaction ID for cursor validation
            this->_header->last_merged_txn_id.store(max_merged_txn_id, std::memory_order_release);
        } else {
            // No merge work, wait for notification or timeout
            std::unique_lock<boost::interprocess::interprocess_mutex> lock(this->_header->background_mutex);
            this->_header->background_cv.wait_for(lock, 
                boost::posix_time::milliseconds(50),
                [&]() { return _shutdown.load(std::memory_order_acquire); });
        }
    }
}

template <typename Storage_>
void _LSMDB<Storage_>::_merge_transaction(txn_ptr& txn) {
    // Iterate through segments (oldest first from segment_head)
    offset_e seg_offset = txn->segment_head;
    while (seg_offset) {
        auto segment = this->template resolve<SegmentType>(seg_offset, WRITE);
        
        // TODO: Implement actual merge logic using _Merger
        // For now, just advance to next segment
        
        offset_e next_seg = segment->next;
        seg_offset = next_seg;
    }
    
    // Transaction state (COMMITTED, sync, refs) handled by _background_loop
}

template <typename Storage_>
void _LSMDB<Storage_>::_prune_tombstones_cycle() {
    // TODO: Implement tombstone pruning
    // This is a lower-priority operation that can be interrupted by merges
}

template <typename Storage_>
void _LSMDB<Storage_>::_sanitize_database() {
    // Walk all transactions and sanitize their state
    std::vector<txn_ptr> txns_to_rollback;
    
    this->iter_transactions([&](txn_ptr txn) -> bool {
        if (txn->merge_phase == WRITING) {
            // Transaction was active during crash - must be rolled back
            txns_to_rollback.push_back(txn);
        } else if (txn->merge_phase == COMMITTING) {
            // Transaction was committed but merge may not have completed
            // Ensure refs is set to protect during merge
            if (txn->refs.load(std::memory_order_acquire) == 0) {
                txn->refs.fetch_add(1, std::memory_order_release);
            }
        }
        
        // Reset segment refcounts (cursors don't survive across process restarts)
        offset_e seg_offset = txn->segment_head;
        while (seg_offset) {
            auto segment = this->template resolve<SegmentType>(seg_offset, WRITE);
            segment->cursor_refs.fetch_add(1, std::memory_order_relaxed);
            seg_offset = segment->next;
        }
        
        return false;  // Continue to next transaction
    });
    
    // Rollback all WRITING transactions
    for (auto& txn : txns_to_rollback) {
        offset_e seg_offset = txn->segment_head;
        while (seg_offset) {
            auto segment = this->template resolve<SegmentType>(seg_offset, WRITE);
            offset_e next_seg = segment->next;
            
            // Free segment - will be recycled
            this->free_block(segment, Transaction::SEGMENT_SIZE);
            
            seg_offset = next_seg;
        }
        
        // Mark transaction for removal
        txn->refs.store(0, std::memory_order_release);
        txn->segment_head = 0;
    }
}

} // namespace leaves

#endif // _LEAVES__LSM_DB_HPP
