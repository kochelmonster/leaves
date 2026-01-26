#ifndef _LEAVES_LSM_HPP
#define _LEAVES_LSM_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "../db/_db.hpp"
#include "../util/_merger.hpp"

namespace leaves {

/**
 * @brief LSM-style database with two levels
 *
 * L0: Write buffer - fast sequential inserts using standard _Inserter
 * L1: Read-optimized store with sorted layout from merge
 *
 * Architecture:
 * - Writes go to L0 (current write buffer)
 * - Background thread merges L0 into L1 when L0 reaches threshold
 * - Reads merge L0 and L1 cursors, preferring L0 for same key
 * - Double buffering: two L0 stores to avoid write stalls during merge
 * - Reference counting: keeps L0 alive while cursors hold references
 */

template <typename Traits_>
struct _LSMCursor;

/**
 * @brief Handler for LSM merge operations - prefer newer values
 */
template <typename CursorDst>
struct _LSMMergeHandler {
  CursorDst& cursor;

  _LSMMergeHandler(CursorDst& c) : cursor(c) {}

  bool operator()(const std::string&, const Slice&, const Slice&) {
    return true;  // Always overwrite with newer value
  }

  template <typename LeafNode>
  void free_big(LeafNode& leaf) {
    // No need to free source big values during merge
  }

  template <typename LeafNode, typename DB>
  Slice migrate_big_value(LeafNode& leaf, DB& db) {
    // Same storage - no migration needed, just return the value
    return leaf.value();
  }
};

/**
 * @brief Header for LSM database
 *
 * Contains references to L0 (double-buffered) and L1 databases
 */
template <typename Storage_>
struct _LSMHeader {
  using Mutex = typename Storage_::Mutex;

  // L0 double buffer: two write buffers that alternate
  offset_t l0_a_header;  // First L0 buffer header offset
  offset_t l0_b_header;  // Second L0 buffer header offset

  // Which L0 is currently active for writes (0 = A, 1 = B)
  std::atomic<uint8_t> active_l0;

  // L1: read-optimized level
  offset_t l1_header;

  // Reference counts for L0 buffers (cursors holding references)
  std::atomic<uint32_t> l0_a_refs;
  std::atomic<uint32_t> l0_b_refs;

  // Lock for merge operations
  Mutex merge_lock;

  // Merge state
  std::atomic<bool> merge_in_progress;

  // Statistics
  std::atomic<uint64_t> l0_size;  // Approximate size of current L0
};

/**
 * @brief LSM Database implementation
 *
 * Wraps two _DB instances (L0 and L1) with LSM semantics
 */
template <typename Storage_,
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename DBHeader_ = _DBHeader<Storage_>>
struct _LSMDB {
  typedef Storage_ Storage;
  typedef Transaction_ Transaction;
  typedef _LSMDB<Storage_, Transaction_, DBHeader_> LSMDB;
  typedef _DB<Storage_, Transaction_, DBHeader_> DB;
  typedef _LSMHeader<Storage_> LSMHeader;
  using Traits = typename Storage::Traits;
  using Mutex = typename Storage::Mutex;
  using offset_e = typename Traits::offset_e;

  using header_ptr = typename Traits::template Pointer<LSMHeader>;

  typedef _LSMCursor<typename DB::CursorTraits> Cursor;
  typedef LSMDB db_type;
  typedef std::shared_ptr<Cursor> cursor_ptr;

  Storage& _storage;
  header_ptr _header;
  uint16_t _index;

  // The actual L0 and L1 databases
  std::unique_ptr<DB> _l0_a;
  std::unique_ptr<DB> _l0_b;
  std::unique_ptr<DB> _l1;

  // Merge threshold (bytes in L0 before triggering merge)
  uint64_t _merge_threshold = 64 * 1024 * 1024;  // 64 MB default

  // Merge thread control
  std::atomic<bool> _shutdown{false};
  std::atomic<bool> _merge_scheduled{false};

  _LSMDB(Storage& storage, offset_t header, uint16_t index)
      : _storage(storage),
        _header(storage.resolve(&header, READ)),
        _index(index) {
    // Initialize L0 and L1 from existing headers - all share parent's index
    _l0_a = std::make_unique<DB>(storage, _header->l0_a_header, index);
    _l0_b = std::make_unique<DB>(storage, _header->l0_b_header, index);
    _l1 = std::make_unique<DB>(storage, _header->l1_header, index);
  }

  _LSMDB(Storage& storage, offset_t* header, uint16_t index)
      : _storage(storage), _index(index) {
    init(header);
  }

  ~_LSMDB() {
    // Signal shutdown - any pending merge task will exit early
    _shutdown.store(true);

    // Wait for any in-progress merge to complete
    while (_header->merge_in_progress.load()) {
      std::this_thread::yield();
    }
  }

  void init(offset_t* header) {
    // Allocate LSM header
    auto area_ptr = _storage.alloc_single_area();
    *header = area_ptr->content_offset();
    _header = _storage.resolve(header, READ);
    memset((char*)&*_header, 0, sizeof(LSMHeader));
    new (&_header->merge_lock) Mutex();

    // Calculate offsets for sub-database headers
    uint16_t header_size = padding(sizeof(LSMHeader), Traits::PAGE_SIZES[0]);
    offset_t base = *header + header_size;

    // Initialize L0 A
    _header->l0_a_header = base;
    _l0_a = std::make_unique<DB>(_storage, &_header->l0_a_header, _index);

    // Initialize L0 B
    _header->l0_b_header = base + Traits::AREA_SIZE;
    _l0_b = std::make_unique<DB>(_storage, &_header->l0_b_header, _index);

    // Initialize L1
    _header->l1_header = base + 2 * Traits::AREA_SIZE;
    _l1 = std::make_unique<DB>(_storage, &_header->l1_header, _index);

    _header->active_l0.store(0);
    _header->l0_a_refs.store(0);
    _header->l0_b_refs.store(0);
    _header->merge_in_progress.store(false);
    _header->l0_size.store(0);

    _storage.make_dirty(_header);
    _storage.flush();
  }

  // Get the currently active L0 for writes
  DB* active_l0() {
    return _header->active_l0.load() == 0 ? _l0_a.get() : _l0_b.get();
  }

  // Get the inactive L0 (being merged or empty)
  DB* inactive_l0() {
    return _header->active_l0.load() == 0 ? _l0_b.get() : _l0_a.get();
  }

  DB* l1() { return _l1.get(); }

  // Increment reference count for a specific L0
  void ref_l0(uint8_t which) {
    if (which == 0)
      _header->l0_a_refs.fetch_add(1);
    else
      _header->l0_b_refs.fetch_add(1);
  }

  // Decrement reference count for a specific L0
  void unref_l0(uint8_t which) {
    if (which == 0)
      _header->l0_a_refs.fetch_sub(1);
    else
      _header->l0_b_refs.fetch_sub(1);
  }

  // Get reference count for inactive L0
  uint32_t inactive_l0_refs() {
    return _header->active_l0.load() == 0 ? _header->l0_b_refs.load()
                                          : _header->l0_a_refs.load();
  }

  cursor_ptr create_cursor() { return std::make_unique<Cursor>(this); }

  const db_type* _internal() const { return this; }

  uint64_t new_cursor_id() { return _storage.new_cursor_id(); }

  Slice name() const { return _storage.db_name(_index); }

  void flush(bool sync = false, bool force = false) {
    _l0_a->flush(sync, force);
    _l0_b->flush(sync, force);
    _l1->flush(sync, force);
  }

  // Check if merge should be triggered
  bool should_merge() const {
    return _header->l0_size.load() >= _merge_threshold &&
           !_header->merge_in_progress.load();
  }

  // Switch active L0 buffer (called when starting merge)
  void switch_l0() {
    uint8_t current = _header->active_l0.load();
    _header->active_l0.store(current == 0 ? 1 : 0);
    _header->l0_size.store(0);
    _storage.make_dirty(_header);
  }

  // Schedule a merge task on the storage's thread pool
  void schedule_merge() {
    if (_merge_scheduled.exchange(true)) return;  // Already scheduled
    if (_shutdown.load()) return;

    _storage.submit_task([this]() {
      if (!_shutdown.load()) {
        do_merge();
      }
      _merge_scheduled.store(false);
    });
  }

  // Perform merge of inactive L0 into L1
  void do_merge() {
    std::scoped_lock lock(_header->merge_lock);
    if (_header->merge_in_progress.load()) return;

    _header->merge_in_progress.store(true);

    // Switch L0 buffers - new writes go to the new buffer
    switch_l0();

    // Note: We do NOT wait for inactive_l0_refs to drop to 0 before merging.
    // The merge reads from the inactive L0 and writes to L1.
    // Cursors that were created during merge_in_progress will have their
    // own cursor on the inactive L0 and can read from it.
    // After merge completes, we wait for all inactive L0 refs to drop
    // before resetting.

    // Get cursors for inactive L0 and L1 in a nested scope
    // so they're destroyed before we try to reset
    {
      DB* old_l0 = inactive_l0();
      auto l0_cursor = old_l0->create_cursor();
      auto l1_cursor = _l1->create_cursor();

      // Merge L0 into L1 using the LSM merge handler
      using L1Cursor = typename DB::Cursor;
      _LSMMergeHandler<L1Cursor> handler(*l1_cursor);

      l0_cursor->first();
      if (l0_cursor->is_valid()) {
        l1_cursor->start_transaction();
        _Merger merger(*l1_cursor, *l0_cursor, handler);

        while (l0_cursor->is_valid()) {
          merger.exec();
          l0_cursor->next();
        }

        l1_cursor->commit();
      }
    }  // l0_cursor and l1_cursor destroyed here

    // Now wait for all cursors reading from inactive L0 to finish
    while (inactive_l0_refs() > 0) {
      if (_shutdown.load()) {
        // Even on shutdown, merge is complete - data is in L1
        // Just can't reset the old L0 yet
        break;
      }
      std::this_thread::yield();
    }

    // Reset the old L0 by reinitializing it (if not shutting down)
    if (!_shutdown.load()) {
      reset_inactive_l0();
    }

    _header->merge_in_progress.store(false);
    _storage.make_dirty(_header);
  }

  // Reset the inactive L0 database after merge
  void reset_inactive_l0() {
    uint8_t active = _header->active_l0.load();
    if (active == 0) {
      // L0 B is inactive, reset it
      _l0_b->reset(&_header->l0_b_header);
    } else {
      // L0 A is inactive, reset it
      _l0_a->reset(&_header->l0_a_header);
    }
    _storage.make_dirty(_header);
  }

  void sanitize() {
    _l0_a->sanitize();
    _l0_b->sanitize();
    _l1->sanitize();
    new (&_header->merge_lock) Mutex();
    _header->merge_in_progress.store(false);

    // Reschedule merge if L0 needs compaction
    if (should_merge()) {
      schedule_merge();
    }
  }
};

/**
 * @brief LSM Cursor that merges L0 and L1 cursors
 *
 * Presents a unified view of both levels, preferring L0 for same key
 */
template <typename Traits_>
struct _LSMCursor {
  typedef Traits_ Traits;
  typedef _LSMCursor<Traits_> LSMCursor;
  typedef typename Traits::DB DB;
  typedef _LSMDB<typename DB::Storage, typename DB::Transaction,
                 _DBHeader<typename DB::Storage>>
      LSMDB;
  typedef _TransactionalCursor<Traits_> InnerCursor;
  using offset_e = typename Traits::offset_e;

  LSMDB* _lsm_db;
  std::shared_ptr<InnerCursor> _l0_cursor;        // Active L0 cursor
  std::shared_ptr<InnerCursor> _l0_inactive_cursor;  // Inactive L0 cursor (during merge)
  std::shared_ptr<InnerCursor> _l1_cursor;
  uint8_t _l0_which;  // Which L0 buffer this cursor is using
  bool _has_inactive_l0{false};  // Whether we have an inactive L0 cursor
  uint64_t _id{0};

  // Which cursor is currently "active" (pointing to current position)
  enum class Active { L0, L0_INACTIVE, L1, BOTH, NONE } _active = Active::NONE;

  _LSMCursor(LSMDB* lsm_db)
      : _lsm_db(lsm_db), _l0_which(lsm_db->_header->active_l0.load()) {
    _id = lsm_db->new_cursor_id();

    // Reference the L0 we're using
    lsm_db->ref_l0(_l0_which);

    // Create cursors for both levels
    DB* l0 = _l0_which == 0 ? lsm_db->_l0_a.get() : lsm_db->_l0_b.get();
    _l0_cursor = l0->create_cursor();
    _l1_cursor = lsm_db->l1()->create_cursor();

    // If merge is in progress, also create cursor for inactive L0
    // We ref-count it so the merge knows not to reset it until we're done
    if (lsm_db->_header->merge_in_progress.load()) {
      uint8_t inactive_which = _l0_which == 0 ? 1 : 0;
      lsm_db->ref_l0(inactive_which);
      DB* inactive_l0 = inactive_which == 0 ? lsm_db->_l0_a.get() : lsm_db->_l0_b.get();
      _l0_inactive_cursor = inactive_l0->create_cursor();
      _has_inactive_l0 = true;
    }
  }

  ~_LSMCursor() {
    // Release L0 reference
    _lsm_db->unref_l0(_l0_which);
    // Release inactive L0 reference if we have one
    if (_has_inactive_l0) {
      uint8_t inactive_which = _l0_which == 0 ? 1 : 0;
      _lsm_db->unref_l0(inactive_which);
    }
  }

  bool is_valid() const {
    return _l0_cursor->is_valid() || _l1_cursor->is_valid() ||
           (_has_inactive_l0 && _l0_inactive_cursor->is_valid());
  }

  Slice key() const {
    switch (_active) {
      case Active::L1:  // Most common case - majority of data is in L1
        return _l1_cursor->key();
      case Active::L0_INACTIVE:
        return _l0_inactive_cursor->key();
      case Active::L0:
      case Active::BOTH:
        return _l0_cursor->key();
      default:
        return Slice();
    }
  }

  Slice value() const {
    switch (_active) {
      case Active::L1:  // Most common case - majority of data is in L1
        return _l1_cursor->value();
      case Active::L0_INACTIVE:
        return _l0_inactive_cursor->value();
      case Active::L0:
      case Active::BOTH:  // L0 takes precedence for overwrites
        return _l0_cursor->value();
      default:
        return Slice();
    }
  }

  void find(const Slice& key) {
    _l0_cursor->find(key);
    if (_has_inactive_l0) _l0_inactive_cursor->find(key);
    _l1_cursor->find(key);
    _update_active();
  }

  void first() {
    _l0_cursor->first();
    if (_has_inactive_l0) _l0_inactive_cursor->first();
    _l1_cursor->first();
    _update_active();
  }

  void last() {
    _l0_cursor->last();
    if (_has_inactive_l0) _l0_inactive_cursor->last();
    _l1_cursor->last();
    _update_active_reverse();
  }

  void next() {
    // Advance whichever cursor(s) are at current position
    switch (_active) {
      case Active::L0:
        _l0_cursor->next();
        break;
      case Active::L0_INACTIVE:
        _l0_inactive_cursor->next();
        break;
      case Active::L1:
        _l1_cursor->next();
        break;
      case Active::BOTH:
        _l0_cursor->next();
        if (_has_inactive_l0 && _l0_inactive_cursor->is_valid() &&
            _l0_inactive_cursor->key() == _l0_cursor->key()) {
          _l0_inactive_cursor->next();
        }
        _l1_cursor->next();
        break;
      default:
        return;
    }
    _update_active();
  }

  void prev() {
    switch (_active) {
      case Active::L0:
        _l0_cursor->prev();
        break;
      case Active::L0_INACTIVE:
        _l0_inactive_cursor->prev();
        break;
      case Active::L1:
        _l1_cursor->prev();
        break;
      case Active::BOTH:
        _l0_cursor->prev();
        if (_has_inactive_l0 && _l0_inactive_cursor->is_valid() &&
            _l0_inactive_cursor->key() == _l0_cursor->key()) {
          _l0_inactive_cursor->prev();
        }
        _l1_cursor->prev();
        break;
      default:
        return;
    }
    _update_active_reverse();
  }

  // Write operations go to L0
  void* reserve(size_t size) {
    // Need to find position in L0 if not already there
    if (_active == Active::L1 || _active == Active::NONE) {
      if (_l1_cursor->is_valid()) {
        _l0_cursor->find(_l1_cursor->key());
      }
    }
    return _l0_cursor->reserve(size);
  }

  void value(const Slice& val) {
    void* space = reserve(val.size());
    memcpy(space, val.data(), val.size());
    _lsm_db->flush();

    // Update L0 size estimate and trigger merge if needed
    _lsm_db->_header->l0_size.fetch_add(val.size());
    if (_lsm_db->should_merge()) {
      _lsm_db->schedule_merge();
    }
  }

  void remove() {
    // For LSM, we could use tombstones, but for now just remove from L0
    // if present, and mark as deleted somehow
    if (_l0_cursor->is_valid()) {
      _l0_cursor->remove();
    }
    // TODO: Handle tombstones for L1 deletes
  }

  bool start_transaction(bool non_blocking = false) {
    return _l0_cursor->start_transaction(non_blocking);
  }

  tid_t prepare_commit(bool sync = false) {
    return _l0_cursor->prepare_commit(sync);
  }

  bool commit(bool sync = false) { return _l0_cursor->commit(sync); }

  bool rollback() { return _l0_cursor->rollback(); }

  // Determine which cursor is "active" based on key comparison
  // Priority: L0 (active) > L0 (inactive) > L1
  void _update_active() {
    bool l0_valid = _l0_cursor->is_valid();
    bool l0_inactive_valid = _has_inactive_l0 && _l0_inactive_cursor->is_valid();
    bool l1_valid = _l1_cursor->is_valid();

    if (!l0_valid && !l0_inactive_valid && !l1_valid) {
      _active = Active::NONE;
      return;
    }

    // Find the smallest key among all valid cursors
    Slice min_key;
    Active min_cursor = Active::NONE;

    if (l0_valid) {
      min_key = _l0_cursor->key();
      min_cursor = Active::L0;
    }

    if (l0_inactive_valid) {
      Slice key = _l0_inactive_cursor->key();
      if (min_cursor == Active::NONE || key.compare(min_key) < 0) {
        min_key = key;
        min_cursor = Active::L0_INACTIVE;
      }
    }

    if (l1_valid) {
      Slice key = _l1_cursor->key();
      if (min_cursor == Active::NONE || key.compare(min_key) < 0) {
        min_key = key;
        min_cursor = Active::L1;
      }
    }

    // Check for ties - prefer L0 > L0_INACTIVE > L1
    if (l0_valid && _l0_cursor->key().compare(min_key) == 0) {
      _active = Active::L0;  // L0 active always wins for equal keys
    } else if (l0_inactive_valid && _l0_inactive_cursor->key().compare(min_key) == 0) {
      _active = Active::L0_INACTIVE;
    } else {
      _active = min_cursor;
    }
  }

  // Same as _update_active but for reverse iteration
  void _update_active_reverse() {
    bool l0_valid = _l0_cursor->is_valid();
    bool l0_inactive_valid = _has_inactive_l0 && _l0_inactive_cursor->is_valid();
    bool l1_valid = _l1_cursor->is_valid();

    if (!l0_valid && !l0_inactive_valid && !l1_valid) {
      _active = Active::NONE;
      return;
    }

    // Find the largest key among all valid cursors
    Slice max_key;
    Active max_cursor = Active::NONE;

    if (l0_valid) {
      max_key = _l0_cursor->key();
      max_cursor = Active::L0;
    }

    if (l0_inactive_valid) {
      Slice key = _l0_inactive_cursor->key();
      if (max_cursor == Active::NONE || key.compare(max_key) > 0) {
        max_key = key;
        max_cursor = Active::L0_INACTIVE;
      }
    }

    if (l1_valid) {
      Slice key = _l1_cursor->key();
      if (max_cursor == Active::NONE || key.compare(max_key) > 0) {
        max_key = key;
        max_cursor = Active::L1;
      }
    }

    // Check for ties - prefer L0 > L0_INACTIVE > L1
    if (l0_valid && _l0_cursor->key().compare(max_key) == 0) {
      _active = Active::L0;  // L0 active always wins for equal keys
    } else if (l0_inactive_valid && _l0_inactive_cursor->key().compare(max_key) == 0) {
      _active = Active::L0_INACTIVE;
    } else {
      _active = max_cursor;
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_LSM_HPP
