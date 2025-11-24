#ifndef _LEAVES__LSM_DB_HPP
#define _LEAVES__LSM_DB_HPP

#include <atomic>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <thread>

#include "_db.hpp"
#include "_memory.hpp"
#include "_memstore.hpp"
#include "_merger.hpp"

namespace leaves {

// Forward declarations
template <typename LSMDB_>
struct Segment;

template <typename LSMDB_>
struct SegmentResolver;

// Merge phase states for LSM transactions
enum MergePhase : uint8_t {
  WRITING = 0,     // Active transaction, writes going to segment
  COMMITTING = 1,  // Committed, ready for merge
  COMMITTED = 2    // Merge complete, transaction fully committed
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
};

// LSM-specific transaction - extends base transaction
template <typename Traits_>
struct _LSMTransaction : public _Transaction<Traits_> {
  typedef _Transaction<Traits_> BaseTransaction;
  using Traits = Traits_;
  using ptr = typename Traits::Pointer<_LSMTransaction>;

  // LSM-specific fields
  offset_t segment_head{0};     // Head of segment linked list (oldest first)
  offset_t current_segment{0};  // Current segment offset being written
  uint8_t merge_phase{WRITING};
  uint8_t sync_commit{
      0};  // 1 = committed with sync, requires sync flush after merge

  // Segment recycling via _GarbageSlot (fixed 64KB segments)
  _GarbageSlot<Traits_> _free_segments;

  static constexpr uint32_t SEGMENT_SIZE = 65536;  // 64KB per segment

  // Override size to include LSM fields
  uint16_t size() const { return sizeof(_LSMTransaction); }

  // Allocate a new segment for this transaction
  template <typename DB>
  offset_t _allocate_segment(DB* db) {
    // Create segment resolver for this transaction
    SegmentResolver<DB> resolver(db);
    typedef typename DB::SegmentType::ptr segment_ptr;

    // Try to pop from recycle pool (transaction-safe)
    segment_ptr pseg = _free_segments.pop(resolver);

    if (!pseg) {
      // Allocate new area from storage if pool empty
      // Split area into segments, last segment gets remaining space
      auto area = db->alloc_single_area();
      offset_t area_start = area->content_offset();
      offset_t area_end = area->end();

      // Split area into segments and push all to _free_segments
      offset_t seg_offset = area_start;
      while (seg_offset < area_end) {
        pseg = db->template resolve(seg_offset, WRITE);

        // Calculate data bounds for this segment
        // data_start accounts for Segment header (includes BaseMemoryDB fields
        // + LSM fields)
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
        pseg->init(data_start, data_end);
        db->make_dirty(pseg);
        _free_segments.push(pseg, resolver);

        seg_offset += SEGMENT_SIZE;
      }

      pseg = _free_segments.pop(resolver);
    }

    assert(pseg);
    pseg->reinit(db);

    // Immediately push back to _GarbageSlot for future recycling
    // SegmentResolver::may_recycle() will check refs and merge_phase
    // when popping
    _free_segments.push(pseg, resolver);

    return db->resolve(pseg);
  }

  // Iterate through all segments in the segment chain
  template <typename DB, typename T>
  void iter_segments(DB* db, T caller) {
    if (!segment_head) return;

    offset_t seg_offset = segment_head;
    while (seg_offset) {
      auto segment =
          db->template resolve<typename DB::SegmentType>(seg_offset, READ);
      if (caller(&*segment, seg_offset)) break;
      seg_offset = segment->next;
    }
  }
};

// Segment represents a fixed-size (64KB) storage block for one transaction's
// writes. Segments work like _MemoryDB - lightweight database objects with
// their own memory manager.
template <typename LSMDB_>
struct Segment {
  typedef LSMDB_ LSMDB;
  typedef Segment<LSMDB> SegmentType;
  typedef typename LSMDB::Transaction Transaction;
  using ptr = typename LSMDB::Traits::template Pointer<SegmentType>;

  struct NullTransaction {
    tid_t txn_id = tid_t(1);
    offset_t root{0};
  };
  typedef NullTransaction* txn_ptr;

  struct SegmentTraits : public LSMDB::Traits {
    typedef LSMDB::Traits BaseTraits;

    // Smaller BlockContainer for segment's limited space (512 bytes instead of
    // 4KB)
    static constexpr size_t BLOCK_CONTAINER_SIZE = 512;

    // Segment traits work like _MemoryDB::ValueTraits
    typedef ::NullHasher Hasher;  // No hashing needed for segments
    typedef uint8_t hash_t[0];
    constexpr static bool TRANSACTIONAL = false;

    typedef Segment::ptr db_ptr;
    // Get/set root from segment's transaction
    template <typename TxnPtr>
    static void set_root(TxnPtr txn, offset_t offset) {
      txn->root = offset;
    }

    template <typename TxnPtr>
    static offset_t get_root(TxnPtr txn) {
      return txn->root;
    }
  };

  // Now define Traits to be SegmentTraits (not LSMDB::Traits)
  typedef SegmentTraits Traits;
  typedef typename Traits::ptr block_ptr;
  typedef _MemManager<Traits> MemManager;
  using area_ptr = typename LSMDB::area_ptr;

  MemManager _mem_manager;
  offset_t allocation_start{0};
  offset_t next{0};  // Next segment in the linked list (offset, not pointer)
  std::atomic<int> refs{0};  // Refcount tracks all cursors using this segment
  LSMDB* parent_db{nullptr};           
  NullTransaction _null_txn;

  // Initialize segment's memory manager with specific bounds
  // Called once when area is split into segments
  void init(offset_t data_start, offset_t data_end) {
    allocation_start = data_start + sizeof(Segment);
    _mem_manager.init(allocation_start, data_end);
    next = 0;
    _null_txn.root = 0;
    refs.store(0, std::memory_order_release);
  }

  // Set parent DB pointer before returning segment to user
  void reinit(LSMDB* parent) {
    _mem_manager.allocation_start = allocation_start;
    next = 0;
    _null_txn.root = 0;
    refs.store(1, std::memory_order_release);
    parent_db = parent;
  }

  txn_ptr txn() { return &_null_txn; }

  // Block allocation (like _MemoryDB::alloc)
  block_ptr alloc(uint16_t space) {
    uint8_t slot = _mem_manager.assign_slot(space);
    return alloc_slot(slot);
  }

  void free(block_ptr p) { _mem_manager.free(p, *this); }

  // Memory manager interface (like _MemoryDB::alloc_slot)
  block_ptr alloc_slot(uint8_t slot_id) {
    return _mem_manager.alloc(slot_id, *this);
  }

  // Methods required by _MemManager (like _MemoryDB)
  template <typename BlockType>
  void mark_for_recycle(const BlockType& /*block*/) {}

  template <typename BlockType>
  bool may_recycle(const BlockType& /*block*/) const {
    return true;
  }

  // Resolve methods - delegate to parent DB
  template <typename T = typename Traits::BlockHeader>
  auto resolve(offset_t offset, Access mode = READ) {
    return parent_db->template resolve<T>(offset, mode);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return parent_db->resolve(p);
  }

  // Make dirty - delegate to parent
  template <typename T>
  void make_dirty(T block) {
    parent_db->make_dirty(block);
  }

  // Prefetch (no-op like _MemoryDB)
  void prefetch(offset_t /*offset*/, Access /*access*/ = READ) const {}
  void prefetch(void* /*mem*/, Access /*access*/ = READ) const {}

  // Flush (no-op like _MemoryDB)
  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}

  // Big allocation - delegate to parent DB
  AreaSlice alloc_big(uint64_t size) { return parent_db->alloc_big(size); }

  void free_big(typename Traits::offset_e offset, size_t size) {
    parent_db->free_big(offset, size);
  }

  area_ptr alloc_single_area() { return area_ptr(); }

  // Copy-on-write - delegate to parent
  template <typename ptr>
  ptr cow(ptr& src) {
    return parent_db->cow(src);
  }

  // Transaction status (segments don't use transaction system)
  tid_t txn_id() const { return _null_txn.txn_id; }
  tid_t transaction_active() const { return tid_t(0); }

  // Cursor management
  void acquire() { refs.fetch_add(1, std::memory_order_relaxed); }
  void release() { refs.fetch_sub(1, std::memory_order_release); }
};

// Custom resolver for segment recycling
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

  // Check if segment can be recycled (refs == 0 and merge complete)
  template <typename BlockItem>
  bool may_recycle(const BlockItem& item) {
    auto segment = db->template resolve<Segment>(item.link, READ);
    return segment->refs.load(std::memory_order_acquire) == 0;
  }

  // Mark segment for recycling with current transaction ID
  template <typename BlockItem>
  void mark_for_recycle(BlockItem& item) {
    // No marking needed - segment state tracked by transaction merge_phase
  }

  // Free a BlockContainer back to the database
  void free(cont_ptr block) { db->free(block); }

  // Allocate a BlockContainer for the GarbageSlot
  cont_ptr alloc_slot(uint16_t slot_id) { return db->alloc_slot(slot_id); }

  void make_dirty(cont_ptr block) { db->make_dirty(block); }
};

// Forward declaration for cursor
template <typename DB_, typename Traits_>
struct _LSMCursor;

// LSM Database - derives from base DB
template <typename Storage_>
struct _LSMDB : public _DB<Storage_, _LSMTransaction<typename Storage_::Traits>,
                           LSMHeader<Storage_>> {
  typedef _DB<Storage_, _LSMTransaction<typename Storage_::Traits>,
              LSMHeader<Storage_>>
      BaseDB;
  typedef _LSMDB<Storage_> DB;
  typedef _LSMTransaction<typename Storage_::Traits> Transaction;
  typedef Segment<_LSMDB<Storage_>> SegmentType;

  struct BaseCursorTraits : public BaseDB::ValueTraits {
    typedef BaseDB* db_ptr;
  };

  struct CursorTraits : public BaseDB::ValueTraits {
    typedef std::shared_ptr<DB> db_ptr;
    static constexpr size_t MAX_KEY_SIZE = 32 * K;
  };
  typedef _LSMCursor<DB, CursorTraits> Cursor;

  using txn_ptr = typename BaseDB::txn_ptr;
  using Traits = typename Storage_::Traits;
  using area_ptr = typename Storage_::area_ptr;
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
    this->_header->last_merged_txn_id.store(tid_t(0),
                                            std::memory_order_release);
    this->_header->auto_commit_ms = 200;
    this->_header->auto_commit_enabled = 0;
    // Initialize additional transaction state
    auto txn = this->txn();
    txn->segment_head = 0;
    txn->current_segment = 0;
    txn->sync_commit = 0;
    txn->merge_phase = COMMITTED;
    this->flush();
    _start_background_thread();
  }

  ~_LSMDB() { _stop_background_thread(); }

  // Auto-commit API (configuration stored in LSMHeader)
  void enable_auto_commit(uint32_t timeout_ms = 200) {
    this->_header->auto_commit_enabled = 1;
    this->_header->auto_commit_ms = timeout_ms;
  }

  void disable_auto_commit() { this->_header->auto_commit_enabled = 0; }

  // Background thread operations
  void _start_background_thread() {
    _shutdown.store(false, std::memory_order_release);
    _background_thread = std::thread([this]() { _background_loop(); });
  }

  void _stop_background_thread() {
    _shutdown.store(true, std::memory_order_release);
    this->_header->background_cv.notify_all();
    if (_background_thread.joinable()) {
      _background_thread.join();
    }
  }

  void _background_loop() {
    while (!_shutdown.load(std::memory_order_acquire)) {
      bool any_sync_commit = false;
      tid_t max_merged_txn_id{0};
      bool merged_any = false;

      // Create cursor with raw pointer to BaseDB (this is already BaseDB*)
      typename Cursor::BaseCursor cursor(this);
      cursor.start_transaction();

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

          _merge_transaction(txn, cursor);

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
        cursor.commit(any_sync_commit);

        // Now release refs on merged transactions - segments can be freed after
        // sync completes
        this->iter_transactions([&](txn_ptr txn) -> bool {
          if (txn->txn_id <= max_merged_txn_id) {
            assert(txn->merge_phase == COMMITTED);
            txn->refs.fetch_sub(1, std::memory_order_release);
          }
          return false;
        });

        // Update last merged transaction ID for cursor validation
        this->_header->last_merged_txn_id.store(max_merged_txn_id,
                                                std::memory_order_release);
      } else {
        // No merge work, wait for notification or timeout
        std::unique_lock<boost::interprocess::interprocess_mutex> lock(
            this->_header->background_mutex);
        this->_header->background_cv.wait_for(
            lock, boost::posix_time::milliseconds(50),
            [&]() { return _shutdown.load(std::memory_order_acquire); });
      }
    }
  }

  struct MergerHandler {
    // Always accept source entries (they are newer)
    bool operator()(const std::string& key, const Slice& dst,
                    const Slice& src) {
      return true;  // Always overwrite
    }
  };

  // LSM operations performed by background thread
  void _merge_transaction(txn_ptr& txn,
                          typename Cursor::BaseCursor& dst_cursor) {
    txn->iter_segments(this, [&](auto segment, offset_t seg_offset) -> bool {
      // segment is already a raw pointer from iter_segments (&*segment)
      typedef std::remove_pointer_t<decltype(segment)> SegmentType;
      typedef typename SegmentType::Traits SegmentTraits;
      typedef _Cursor<SegmentType, SegmentTraits> SrcCursor;

      // segment is already a raw pointer
      SrcCursor src_cursor(segment);
      src_cursor._txn = segment->txn(); // set transaction for cursor
      MergerHandler handler;
      _Merger merger(dst_cursor, src_cursor, handler);
      merger.exec();
      return false;  // Continue to next segment
    });

    dst_cursor.commit();
  }

  void _prune_tombstones_cycle() {
    // Check if merge work arrived (check for COMMITTING transactions)
    auto check_merge_work = [&]() {
      bool has_merge_work = false;
      this->iter_transactions([&](txn_ptr txn) -> bool {
        if (txn->merge_phase == COMMITTING) {
          has_merge_work = true;
          return true;  // Break iteration
        }
        return false;
      });
      return has_merge_work;
    };

    if (check_merge_work()) return;  // Interrupt pruning

    // Scan persistent storage for tombstones and remove them
    // Use BaseDB cursor (not LSMCursor) to scan only persistent storage
    typename Cursor::BaseCursor cursor(this);
    cursor.first();

    while (cursor.is_valid()) {
      if (check_merge_work()) {
        cursor.commit();  // Commit any removes done so far before interrupting
        return;           // Interrupt pruning
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

  void sanitize() {
    // Create cursor for sanitization with raw pointer
    typename Cursor::BaseCursor cursor(this);

    // Walk all transactions and sanitize their state
    this->iter_transactions([&](txn_ptr txn) -> bool {
      if (txn->merge_phase == COMMITTING) {
        // Transaction was committed but merge may not have completed
        // Merge it now (no cursors exist yet at startup)
        _merge_transaction(txn, cursor);
        txn->merge_phase = COMMITTED;
      }
      return false;
    });

    cursor.commit();

    // Reset all segment refs to 0 for recycling eligibility
    txn_ptr read_txn =
        this->template resolve<Transaction>(this->_header->read_txn);
    read_txn->_free_segments.iter(*this, [&](auto& block_item) {
      auto segment =
          this->template resolve<SegmentType>(block_item.link, WRITE);
      segment->refs.store(0, std::memory_order_relaxed);
    });

    BaseDB::sanitize();
  }

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
    this->_wtxn->iter_segments(
        this, [&](auto segment, offset_t seg_offset) -> bool {
          segment->refs.store(0, std::memory_order_release);
          return false;
        });
    BaseDB::rollback(cursor_id);
  }

  // Add a new segment to the given transaction (for overflow handling)
  offset_t add_segment(txn_ptr& txn) {
    // Allocate new segment
    offset_t new_segment_offset = txn->_allocate_segment(this);
    auto new_segment =
        this->template resolve<SegmentType>(new_segment_offset, WRITE);

    // Link new segment at the head (becomes current segment)
    new_segment->next = txn->current_segment;
    txn->current_segment = new_segment_offset;

    return new_segment_offset;
  }

  // Override prepare_commit to flush segments in sync mode
  tid_t prepare_commit(uint64_t cursor_id, bool sync = false) {
    // Call base prepare_commit to update prepared_txn and next_txn
    tid_t result = BaseDB::prepare_commit(cursor_id, false);

    // In sync mode, flush segments before preparing transaction
    // This ensures segment data is durable before the transaction becomes
    // visible
    if (result && sync) {
      // Flush all segments in this transaction
      this->_wtxn->iter_segments(
          this, [&](auto segment, offset_t seg_offset) -> bool {
            // Flush only the used portion (allocation_start to allocation_end)
            offset_t start = segment->allocation_start;
            offset_t end = segment->_mem_manager.allocation_start;
            size_t used_size = end - start;
            this->flush(&*segment, start, used_size, true);  // sync=true
            segment->release();
            return false;  // Continue to next segment
          });

      // Flush prepared_txn pointer, read_txn->next_txn, and the whole _wtxn
      area_ptr area =
          this->_storage.resolve(this->_header->area_list_head_single);
      offset_t offset =
          area->content_offset() +
          ((char*)&this->_header->prepared_txn - (char*)&this->_header);
      this->flush(&this->_header->prepared_txn, offset, sizeof(offset_t),
                  true);  // prepared_txn field

      txn_ptr read_txn =
          this->template resolve<Transaction>(this->_header->read_txn);
      offset = this->_header->read_txn +
               ((char*)&read_txn->next_txn - (char*)read_txn);
      this->flush(&read_txn->next_txn, offset, sizeof(offset_t),
                  true);  // next_txn field

      offset = this->_storage.resolve(this->_wtxn);
      this->flush(&*this->_wtxn, offset, sizeof(Transaction),
                  true);  // whole transaction
    }

    return result;
  }

  // Override commit to mark transaction as ready for merge
  bool commit(uint64_t cursor_id, bool sync = false) {
    if (!prepare_commit(cursor_id, sync)) return false;

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
    BaseDB::commit(cursor_id, false);

    if (sync) {
      // Flush read_txn (last committed transaction)
      area_ptr area =
          this->_storage.resolve(this->_header->area_list_head_single);
      offset_t offset =
          area->content_offset() +
          ((char*)&this->_header->read_txn - (char*)&this->_header);
      this->flush(&this->_header->read_txn, offset, sizeof(offset_t), true);
    }
    return true;
  }
};

}  // namespace leaves

#endif  // _LEAVES__LSM_DB_HPP
