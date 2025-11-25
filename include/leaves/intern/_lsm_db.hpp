#ifndef _LEAVES__LSM_DB_HPP
#define _LEAVES__LSM_DB_HPP

#include <atomic>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <thread>
#include <memory>

#include "_db.hpp"
#include "_memory.hpp"
#include "_memstore.hpp"
#include "_merger.hpp"

namespace leaves {

// Forward declarations
template <typename Storage_>
struct _LSMDB;

template <typename LSMDB_>
struct Segment;

template <typename LSMDB_>
struct SegmentResolver;

// LSM Transaction - simplified structure for write transactions
template <typename Traits_>
struct _LSMTransaction {
  using Traits = Traits_;
  using ptr = typename Traits::Pointer<_LSMTransaction>;

  tid_t txn_id{0};
  offset_t segment_head{0};     // Head of segment linked list (oldest first)
  offset_t current_segment{0};  // Current segment offset being written
  uint8_t merge_phase{COMMITTED};
  uint8_t sync_commit{0};  // 1 = committed with sync
  std::atomic<int> refs{0};

  // Segment recycling via _GarbageSlot (fixed 64KB segments)
  _GarbageSlot<Traits_> _free_segments;

  static constexpr uint32_t SEGMENT_SIZE = 65536;  // 64KB per segment

  uint16_t size() const { return sizeof(_LSMTransaction); }

  // Allocate a new segment for this transaction
  template <typename LSMDB>
  offset_t _allocate_segment(LSMDB* lsmdb) {
    // Create segment resolver for this transaction
    SegmentResolver<LSMDB> resolver(lsmdb);
    typedef typename LSMDB::SegmentType::ptr segment_ptr;

    // Try to pop from recycle pool
    segment_ptr pseg = _free_segments.pop(resolver);

    if (!pseg) {
      // Allocate new area from base storage
      auto area = lsmdb->_base_db->alloc_single_area();
      offset_t area_start = area->content_offset();
      offset_t area_end = area->end();

      // Split area into segments
      offset_t seg_offset = area_start;
      while (seg_offset < area_end) {
        pseg = lsmdb->_base_db->template resolve(seg_offset, WRITE);

        offset_t data_start = seg_offset;
        offset_t data_end;

        if (seg_offset + SEGMENT_SIZE <= area_end) {
          data_end = seg_offset + SEGMENT_SIZE;
        } else {
          data_end = area_end;
        }

        pseg->init(data_start, data_end);
        lsmdb->_base_db->make_dirty(pseg);
        _free_segments.push(pseg, resolver);

        seg_offset += SEGMENT_SIZE;
      }

      pseg = _free_segments.pop(resolver);
    }

    assert(pseg);
    pseg->reinit(lsmdb);
    _free_segments.push(pseg, resolver);

    return lsmdb->_base_db->resolve(pseg);
  }

  // Iterate through all segments in the segment chain
  template <typename LSMDB, typename T>
  void iter_segments(LSMDB* lsmdb, T caller) {
    if (!segment_head) return;

    offset_t seg_offset = segment_head;
    while (seg_offset) {
      auto segment =
          lsmdb->_base_db->template resolve<typename LSMDB::SegmentType>(seg_offset, READ);
      if (caller(&*segment, seg_offset)) break;
      seg_offset = segment->next;
    }
  }
};

// Merge phase states
enum MergePhase : uint8_t {
  WRITING = 0,
  COMMITTING = 1,
  COMMITTED = 2
};

// Segment represents a fixed-size (64KB) storage block for one transaction's writes
template <typename LSMDB_>
struct Segment {
  typedef LSMDB_ LSMDB;
  typedef Segment<LSMDB> SegmentType;
  typedef typename LSMDB::LSMTransaction LSMTransaction;
  using ptr = typename LSMDB::Traits::template Pointer<SegmentType>;

  struct NullTransaction {
    tid_t txn_id = tid_t(1);
    offset_t root{0};
  };
  typedef NullTransaction* txn_ptr;

  struct SegmentTraits : public LSMDB::Traits {
    typedef typename LSMDB::Traits BaseTraits;
    static constexpr size_t BLOCK_CONTAINER_SIZE = 512;

    typedef ::NullHasher Hasher;
    typedef uint8_t hash_t[0];
    constexpr static bool TRANSACTION_REF = false;
    constexpr static bool COW = false;

    typedef typename Segment::ptr db_ptr;

    template <typename TxnPtr>
    static void set_root(TxnPtr txn, offset_t offset) {
      txn->root = offset;
    }

    template <typename TxnPtr>
    static offset_t get_root(TxnPtr txn) {
      return txn->root;
    }
  };

  typedef SegmentTraits Traits;
  typedef typename Traits::ptr block_ptr;
  typedef _MemManager<Traits> MemManager;
  using area_ptr = typename LSMDB::area_ptr;

  MemManager _mem_manager;
  offset_t allocation_start{0};
  offset_t next{0};
  std::atomic<int> refs{0};
  LSMDB* parent_db{nullptr};
  NullTransaction _null_txn;

  void init(offset_t data_start, offset_t data_end) {
    allocation_start = data_start + sizeof(Segment);
    _mem_manager.init(allocation_start, data_end);
    next = 0;
    _null_txn.root = 0;
    refs.store(0, std::memory_order_release);
  }

  void reinit(LSMDB* parent) {
    _mem_manager.allocation_start = allocation_start;
    next = 0;
    _null_txn.root = 0;
    refs.store(1, std::memory_order_release);
    parent_db = parent;
  }

  txn_ptr txn() { return &_null_txn; }

  block_ptr alloc(uint16_t space) {
    uint8_t slot = _mem_manager.assign_slot(space);
    return alloc_slot(slot);
  }

  void free(block_ptr p) { _mem_manager.free(p, *this); }

  block_ptr alloc_slot(uint8_t slot_id) {
    return _mem_manager.alloc(slot_id, *this);
  }

  template <typename BlockType>
  void mark_for_recycle(const BlockType& /*block*/) {}

  template <typename BlockType>
  bool may_recycle(const BlockType& /*block*/) const {
    return true;
  }

  template <typename T = typename Traits::BlockHeader>
  auto resolve(offset_t offset, Access mode = READ) {
    return parent_db->_base_db->template resolve<T>(offset, mode);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return parent_db->_base_db->resolve(p);
  }

  template <typename T>
  void make_dirty(T block) {
    parent_db->_base_db->make_dirty(block);
  }

  void prefetch(offset_t /*offset*/, Access /*access*/ = READ) const {}
  void prefetch(void* /*mem*/, Access /*access*/ = READ) const {}

  void flush(bool /*sync*/ = false, bool /*force*/ = false) {}

  // Mock big value handling for now
  AreaSlice alloc_big(uint64_t size) { 
    // TODO: Implement proper big value handling
    return AreaSlice();
  }

  void free_big(typename Traits::offset_e offset, size_t size) {
    // TODO: Implement proper big value handling
  }

  area_ptr alloc_single_area() { return area_ptr(); }

  template <typename ptr>
  ptr clone(ptr& src) {
    return parent_db->_base_db->clone(src);
  }

  tid_t txn_id() const { return _null_txn.txn_id; }
  tid_t transaction_active() const { return tid_t(0); }

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

  LSMDB* lsmdb;

  SegmentResolver(LSMDB* lsmdb_) : lsmdb(lsmdb_) {}

  block_ptr resolve(offset_t offset, Access mode) {
    return lsmdb->_base_db->template resolve<Segment>(offset, mode);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return lsmdb->_base_db->resolve(p);
  }

  template <typename BlockItem>
  bool may_recycle(const BlockItem& item) {
    auto segment = lsmdb->_base_db->template resolve<Segment>(item.link, READ);
    return segment->refs.load(std::memory_order_acquire) == 0;
  }

  template <typename BlockItem>
  void mark_for_recycle(BlockItem& item) {}

  void free(cont_ptr block) { lsmdb->_base_db->free(block); }

  cont_ptr alloc_slot(uint16_t slot_id) { return lsmdb->_base_db->alloc_slot(slot_id); }

  void make_dirty(cont_ptr block) { lsmdb->_base_db->make_dirty(block); }
};

// Forward declaration for cursor
template <typename DB_, typename Traits_>
struct _LSMCursor;

// LSMDB - facade over contained _DB instance with own transaction management
template <typename Storage_>
struct _LSMDB {
  typedef Storage_ Storage;
  typedef typename Storage::Traits Traits;
  typedef _DB<Storage, _Transaction<Traits>, _DBHeader<Storage>> BaseDB;
  typedef _LSMDB<Storage_> DB;
  typedef _LSMTransaction<Traits> LSMTransaction;
  typedef Segment<_LSMDB<Storage_>> SegmentType;

  using txn_ptr = typename BaseDB::txn_ptr;
  using area_ptr = typename Storage::area_ptr;
  using block_ptr = typename Storage::ptr;

  struct CursorTraits : public Traits {
    typedef std::shared_ptr<DB> db_ptr;
    static constexpr size_t MAX_KEY_SIZE = 32 * K;
  };
  typedef _LSMCursor<DB, CursorTraits> Cursor;

  // Contained base storage (the last LSM level)
  std::unique_ptr<BaseDB> _base_db;

  // LSM-specific state (process-local)
  std::vector<std::unique_ptr<LSMTransaction>> _write_transactions;
  std::atomic<tid_t> _next_txn_id{1};
  std::atomic<tid_t> _last_merged_txn_id{0};
  
  // Background thread
  std::thread _background_thread;
  std::atomic<bool> _shutdown{false};
  std::mutex _background_mutex;
  std::condition_variable _background_cv;

  // Auto-commit configuration
  uint32_t _auto_commit_ms{200};
  bool _auto_commit_enabled{false};

  _LSMDB(Storage& storage, offset_t header, uint16_t index)
      : _base_db(std::make_unique<BaseDB>(storage, header, index)) {
    _start_background_thread();
  }

  _LSMDB(Storage& storage, offset_t* header, uint16_t index)
      : _base_db(std::make_unique<BaseDB>(storage, header, index)) {
    _start_background_thread();
  }

  ~_LSMDB() { _stop_background_thread(); }

  // Auto-commit API
  void enable_auto_commit(uint32_t timeout_ms = 200) {
    _auto_commit_enabled = true;
    _auto_commit_ms = timeout_ms;
  }

  void disable_auto_commit() { _auto_commit_enabled = false; }

  // Transaction management - independent from base DB
  std::shared_ptr<LSMTransaction> start_transaction() {
    auto txn = std::make_shared<LSMTransaction>();
    txn->txn_id = _next_txn_id.fetch_add(1, std::memory_order_relaxed);
    txn->merge_phase = WRITING;
    txn->refs.store(1, std::memory_order_release);
    
    // Allocate first segment
    txn->current_segment = txn->_allocate_segment(this);
    txn->segment_head = txn->current_segment;
    
    _write_transactions.push_back(std::move(txn));
    return _write_transactions.back();
  }

  void rollback_transaction(std::shared_ptr<LSMTransaction>& txn) {
    txn->iter_segments(this, [&](auto segment, offset_t seg_offset) -> bool {
      segment->refs.store(0, std::memory_order_release);
      return false;
    });
    
    // Remove from write transactions
    _write_transactions.erase(
        std::remove_if(_write_transactions.begin(), _write_transactions.end(),
                       [&](const auto& t) { return t->txn_id == txn->txn_id; }),
        _write_transactions.end());
  }

  void commit_transaction(std::shared_ptr<LSMTransaction>& txn, bool sync = false) {
    txn->merge_phase = COMMITTING;
    txn->sync_commit = sync ? 1 : 0;
    txn->refs.fetch_add(1, std::memory_order_release); // Keep alive during merge
    
    _background_cv.notify_one();
  }

  // Add a new segment to a transaction
  offset_t add_segment(std::shared_ptr<LSMTransaction>& txn) {
    offset_t new_segment_offset = txn->_allocate_segment(this);
    auto new_segment = _base_db->template resolve<SegmentType>(new_segment_offset, WRITE);
    
    new_segment->next = txn->current_segment;
    txn->current_segment = new_segment_offset;
    
    return new_segment_offset;
  }

  // Background thread operations
  void _start_background_thread() {
    _shutdown.store(false, std::memory_order_release);
    _background_thread = std::thread([this]() { _background_loop(); });
  }

  void _stop_background_thread() {
    _shutdown.store(true, std::memory_order_release);
    _background_cv.notify_all();
    if (_background_thread.joinable()) {
      _background_thread.join();
    }
  }

  void _background_loop() {
    while (!_shutdown.load(std::memory_order_acquire)) {
      bool any_sync_commit = false;
      tid_t max_merged_txn_id{0};
      bool merged_any = false;

      // Create base cursor for merging
      typedef _Cursor<BaseDB, typename BaseDB::ValueTraits> BaseCursor;
      BaseCursor dst_cursor(_base_db.get());
      dst_cursor.start_transaction();

      // Walk write transactions
      for (auto& txn : _write_transactions) {
        if (txn->merge_phase == COMMITTING) {
          if (txn->sync_commit) {
            any_sync_commit = true;
          }

          _merge_transaction(txn, dst_cursor);

          txn->merge_phase = COMMITTED;
          if (txn->txn_id > max_merged_txn_id) {
            max_merged_txn_id = txn->txn_id;
          }

          merged_any = true;
        } else if (txn->merge_phase == WRITING) {
          break; // Can't merge beyond active write
        }
      }

      if (merged_any) {
        dst_cursor.commit(any_sync_commit);

        // Release refs and remove completed transactions
        _write_transactions.erase(
            std::remove_if(_write_transactions.begin(), _write_transactions.end(),
                           [&](const auto& txn) {
                             if (txn->txn_id <= max_merged_txn_id) {
                               txn->refs.fetch_sub(1, std::memory_order_release);
                               return true;
                             }
                             return false;
                           }),
            _write_transactions.end());

        _last_merged_txn_id.store(max_merged_txn_id, std::memory_order_release);
      } else {
        // Wait for work
        std::unique_lock<std::mutex> lock(_background_mutex);
        _background_cv.wait_for(lock, std::chrono::milliseconds(50),
                                [&]() { return _shutdown.load(std::memory_order_acquire); });
      }
    }
  }

  struct MergerHandler {
    bool operator()(const std::string& key, const Slice& dst, const Slice& src) {
      return true;  // Always overwrite
    }
  };

  template <typename BaseCursor>
  void _merge_transaction(std::shared_ptr<LSMTransaction>& txn, BaseCursor& dst_cursor) {
    txn->iter_segments(this, [&](auto segment, offset_t seg_offset) -> bool {
      typedef std::remove_pointer_t<decltype(segment)> SegmentType;
      typedef typename SegmentType::Traits SegmentTraits;
      typedef _Cursor<SegmentType, SegmentTraits> SrcCursor;

      SrcCursor src_cursor(segment);
      src_cursor._txn = segment->txn();
      MergerHandler handler;
      _Merger merger(dst_cursor, src_cursor, handler);
      merger.exec();
      return false;
    });
  }

  void sanitize() {
    // Merge any committed transactions
    typedef _Cursor<BaseDB, typename BaseDB::ValueTraits> BaseCursor;
    BaseCursor cursor(_base_db.get());
    cursor.start_transaction();

    for (auto& txn : _write_transactions) {
      if (txn->merge_phase == COMMITTING) {
        _merge_transaction(txn, cursor);
        txn->merge_phase = COMMITTED;
      }
    }

    cursor.commit();

    // Clean up completed transactions
    _write_transactions.erase(
        std::remove_if(_write_transactions.begin(), _write_transactions.end(),
                       [](const auto& txn) { return txn->merge_phase == COMMITTED; }),
        _write_transactions.end());

    _base_db->sanitize();
  }

  // Delegate base storage methods
  template <typename T = typename Traits::BlockHeader>
  auto resolve(offset_t offset, Access mode = READ) {
    return _base_db->template resolve<T>(offset, mode);
  }

  template <typename Pointer>
  offset_t resolve(const Pointer& p) const {
    return _base_db->resolve(p);
  }

  template <typename T>
  void make_dirty(T block) {
    _base_db->make_dirty(block);
  }

  void flush(bool sync = false) {
    _base_db->flush(sync);
  }

  area_ptr alloc_single_area() {
    return _base_db->alloc_single_area();
  }

  block_ptr alloc(uint16_t size) {
    return _base_db->alloc(size);
  }

  template <typename ptr>
  void free(ptr p) {
    _base_db->free(p);
  }

  block_ptr alloc_slot(uint8_t slot_id) {
    return _base_db->alloc_slot(slot_id);
  }
};

}  // namespace leaves

#endif  // _LEAVES__LSM_DB_HPP
