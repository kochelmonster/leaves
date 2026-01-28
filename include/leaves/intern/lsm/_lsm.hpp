/*
Todos: 
  - Löschen -> node zombies
  - big values -> beide l0 sollen mit dem gleichen root arbeiten
  - was ist mit rollback? Kein l1 commit nach merge. 
       Eine Haupttransaction, die die roots für l0 hält
  
  
  _lsm_db->flush();

  while (_header->merge_in_progress.load()) {
      std::this_thread::yield();
    }

  */

#ifndef _LEAVES_LSM_HPP
#define _LEAVES_LSM_HPP

#include <atomic>
#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../db/_db.hpp"
#include "../db/_check.hpp"
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
 * - Reference counting: embedded in L0 extended header
 * - Zombie L0s: when L0 can't be reset due to refs, marked as zombie and replaced
 */

template <typename Traits_>
struct _LSMCursor;

/**
 * @brief Extended DB header for L0 databases with embedded reference counting
 *
 * Each L0 tracks its own refs and zombie state in its header.
 */
template <typename Storage_>
struct _L0DBHeader : public _DBHeader<Storage_> {
  std::atomic<uint32_t> refs{0};      // Cursor reference count
  std::atomic<bool> is_zombie{false}; // True when replaced but still has refs
};

/**
 * @brief Handler for LSM merge operations - prefer newer values
 */
template <typename CursorDst>
struct _LSMMergePolicy {
  CursorDst& cursor;

  _LSMMergePolicy(CursorDst& c) : cursor(c) {}

  bool check_overwrite(const std::string&, const Slice&, const Slice&) {
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
 * Contains references to L0 (double-buffered) and L1 databases.
 * L0 ref counts are embedded in each L0's _L0DBHeader.
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
 * Wraps two _DB instances (L0 and L1) with LSM semantics.
 * L0 databases use extended header with embedded ref counts.
 */
template <typename Storage_,
          typename Transaction_ = _Transaction<typename Storage_::Traits>,
          typename DBHeader_ = _DBHeader<Storage_>>
struct _LSMDB {
  typedef Storage_ Storage;
  typedef Transaction_ Transaction;
  typedef _LSMDB<Storage_, Transaction_, DBHeader_> LSMDB;
  typedef _DB<Storage_, Transaction_, DBHeader_> DB;
  // L0 uses extended header with embedded refs
  typedef _L0DBHeader<Storage_> L0DBHeader;
  typedef _DB<Storage_, Transaction_, L0DBHeader> L0DB;
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

  // The actual L0 (with extended header) and L1 databases
  std::unique_ptr<L0DB> _l0_a;
  std::unique_ptr<L0DB> _l0_b;
  std::unique_ptr<DB> _l1;

  // Zombie L0s - kept alive until refs drop to 0
  std::vector<std::unique_ptr<L0DB>> _zombies;
  std::mutex _zombie_mutex;

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
    _l0_a = std::make_unique<L0DB>(storage, _header->l0_a_header, index);
    _l0_b = std::make_unique<L0DB>(storage, _header->l0_b_header, index);
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

    // Initialize L0 A (with extended header)
    _header->l0_a_header = base;
    _l0_a = std::make_unique<L0DB>(_storage, &_header->l0_a_header, _index);

    // Initialize L0 B (with extended header)
    _header->l0_b_header = base + Traits::AREA_SIZE;
    _l0_b = std::make_unique<L0DB>(_storage, &_header->l0_b_header, _index);

    // Initialize L1
    _header->l1_header = base + 2 * Traits::AREA_SIZE;
    _l1 = std::make_unique<DB>(_storage, &_header->l1_header, _index);

    _header->active_l0.store(0);
    _header->merge_in_progress.store(false);
    _header->l0_size.store(0);

    _storage.make_dirty(_header);
    _storage.flush();
  }

  // Get the currently active L0 for writes
  L0DB* active_l0() {
    return _header->active_l0.load() == 0 ? _l0_a.get() : _l0_b.get();
  }

  // Get the inactive L0 (being merged or empty)
  L0DB* inactive_l0() {
    return _header->active_l0.load() == 0 ? _l0_b.get() : _l0_a.get();
  }

  DB* l1() { return _l1.get(); }

  // Increment reference count for an L0 (uses embedded header refs)
  void ref_l0(L0DB* l0) {
    l0->_header->refs.fetch_add(1);
  }

  // Decrement reference count for an L0
  // If refs drop to 0 and it's a zombie, schedule cleanup
  void unref_l0(L0DB* l0) {
    if (l0->_header->refs.fetch_sub(1) == 1) {
      // Refs dropped to 0
      if (l0->_header->is_zombie.load()) {
        schedule_zombie_cleanup(l0);
      }
    }
  }
  // Schedule cleanup of a zombie L0
  void schedule_zombie_cleanup(L0DB* l0) {
    _storage.submit_task([this, l0]() {
      cleanup_zombie(l0);
    });
  }

  // Clean up a zombie L0: return its areas, remove from zombie list
  void cleanup_zombie(L0DB* zombie) {
    std::lock_guard lock(_zombie_mutex);
    
    // Find and remove from zombie list
    auto it = std::find_if(_zombies.begin(), _zombies.end(),
        [zombie](const std::unique_ptr<L0DB>& z) { return z.get() == zombie; });
    
    if (it != _zombies.end()) {
      (*it)->return_areas();
      _zombies.erase(it);
    }
  }

  // Prepare inactive L0 for reuse: either reset it or zombify it
  // Returns true if a zombie was created
  bool prepare_inactive_l0() {
    L0DB* inactive = inactive_l0();
    
    // Check if inactive L0 has any refs
    uint32_t refs = inactive->_header->refs.load();
    
    if (refs == 0) {
      // No refs - we can reset it directly
      uint8_t active = _header->active_l0.load();
      if (active == 0) {
        _l0_b->reset(&_header->l0_b_header);
      } else {
        _l0_a->reset(&_header->l0_a_header);
      }
      _storage.make_dirty(_header);
      return false;
    } else {
      // Has refs - mark as zombie and create fresh L0
      zombify_inactive_l0();
      return true;
    }
  }

  // Mark inactive L0 as zombie and create fresh replacement
  void zombify_inactive_l0() {
    std::lock_guard lock(_zombie_mutex);
    
    uint8_t active = _header->active_l0.load();
    
    // Create new L0 BEFORE moving old one to zombie list
    auto new_area = _storage.alloc_single_area();
    offset_t new_header = new_area->content_offset();
    auto new_db = std::make_unique<L0DB>(_storage, &new_header, _index);
    
    if (active == 0) {
      // B is inactive - zombify it
      _l0_b->_header->is_zombie.store(true);
      _zombies.push_back(std::move(_l0_b));
      _l0_b = std::move(new_db);
      _header->l0_b_header = new_header;
    } else {
      // A is inactive - zombify it
      _l0_a->_header->is_zombie.store(true);
      _zombies.push_back(std::move(_l0_a));
      _l0_a = std::move(new_db);
      _header->l0_a_header = new_header;
    }
    _storage.make_dirty(_header);
  }

  cursor_ptr create_cursor() { return std::make_unique<Cursor>(this); }

  const db_type* _internal() const { return this; }

  uint64_t new_cursor_id() { return _storage.new_cursor_id(); }

  Slice name() const { return _storage.db_name(_index); }

  void flush(bool sync = false, bool force = false) {
    // L0 databases might be null during zombie transition
    if (_l0_a) _l0_a->flush(sync, force);
    if (_l0_b) _l0_b->flush(sync, force);
    if (_l1) _l1->flush(sync, force);
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
  // This is called from the writing thread when merge threshold is reached.
  // Returns true if the future active L0 was zombified (caller should recreate cursor)
  // Flow:
  // 1. Wait for any previous merge to complete
  // 2. Prepare the inactive L0 (reset or zombify) - it will become active
  // 3. Mark merge_in_progress BEFORE switching (so new cursors see both L0s)
  // 4. Switch L0 buffers - new writes go to the fresh L0
  // 5. Start background merge of the now-inactive L0 into L1
  bool schedule_merge() {
    if (_merge_scheduled.exchange(true)) return false;  // Already scheduled
    if (_shutdown.load()) return false;

    // 1. Wait for previous merge to complete
    while (_header->merge_in_progress.load()) {
      std::this_thread::yield();
    }

    // 2. Prepare the inactive L0 (which will become active after switch)
    // This resets it if no refs, or zombifies it if cursors still hold refs
    bool zombified = prepare_inactive_l0();

    // 3. Mark merge as in progress BEFORE switching
    // This ensures any cursor created after this point will see both L0s
    _header->merge_in_progress.store(true);
    _storage.make_dirty(_header);

    // 4. Switch L0 buffers - new writes now go to the fresh (just prepared) L0
    switch_l0();

    // 5. Start background merge task
    _storage.submit_task([this]() {
      if (!_shutdown.load()) {
        do_merge();
      }
      _merge_scheduled.store(false);
    });

    return zombified;
  }

  // Perform merge of inactive L0 into L1 (runs on background thread)
  // The L0 switch has already happened - we just merge the inactive L0.
  void do_merge() {
    std::scoped_lock lock(_header->merge_lock);

    // Get the L0 that we're merging FROM (the now-inactive one with data)
    L0DB* merge_source_l0 = inactive_l0();

    // Merge L0 into L1
    {
      auto l1_cursor = _l1->create_cursor();

      using L1Cursor = typename DB::Cursor;
      _LSMMergePolicy<L1Cursor> handler(*l1_cursor);

      // Get source root offset
      offset_e* l0_root = &merge_source_l0->txn()->root;
      if (*l0_root) {  // Only merge if L0 has data
        l1_cursor->start_transaction();

        // Get destination root offset (from current transaction)
        offset_e* l1_root = &_l1->txn()->root;

        // Use new tree-based merge API
        _Merger merger(*_l1, l1_root, *merge_source_l0, l0_root, handler);
        merger.exec();

        l1_cursor->commit();
      }
    }  // l1_cursor destroyed here

    // Merge complete
    _header->merge_in_progress.store(false);
    _storage.make_dirty(_header);
  }

  void sanitize() {
    _l0_a->sanitize();
    _l0_b->sanitize();
    _l1->sanitize();
    new (&_header->merge_lock) Mutex();
    _header->merge_in_progress.store(false);

    // After crash, clear any zombie flags and reset refs
    _l0_a->_header->refs.store(0);
    _l0_a->_header->is_zombie.store(false);
    _l0_b->_header->refs.store(0);
    _l0_b->_header->is_zombie.store(false);
    
    // Clear in-memory zombie list and reclaim their storage
    {
      std::lock_guard lock(_zombie_mutex);
      for (auto& zombie : _zombies) {
        zombie->return_areas();
      }
      _zombies.clear();
    }

    // Reschedule merge if L0 needs compaction
    if (should_merge()) {
      schedule_merge();
    }
  }
};

/**
 * @brief LSM Cursor that merges L0 and L1 cursors
 *
 * Presents a unified view of both levels, preferring L0 for same key.
 * Uses L0DB pointers with embedded ref counts for lifecycle management.
 */
template <typename Traits_>
struct _LSMCursor {
  typedef Traits_ Traits;
  typedef _LSMCursor<Traits_> LSMCursor;
  typedef typename Traits::DB DB;
  typedef _LSMDB<typename DB::Storage, typename DB::Transaction,
                 _DBHeader<typename DB::Storage>>
      LSMDB;
  typedef typename LSMDB::L0DB L0DB;
  // Separate cursor types for L0 (extended header) and L1 (base header)
  typedef _TransactionalCursor<typename L0DB::CursorTraits> L0Cursor;
  typedef _TransactionalCursor<Traits_> L1Cursor;
  using offset_e = typename Traits::offset_e;

  LSMDB* _lsm_db;
  std::shared_ptr<L0Cursor> _l0_a_cursor;
  std::shared_ptr<L0Cursor> _l0_b_cursor;
  L0Cursor* _l0_active;    // Points to active L0 cursor
  L0Cursor* _l0_inactive;  // Points to inactive L0 cursor
  std::shared_ptr<L1Cursor> _l1_cursor;
  L0DB* _l0_a_db{nullptr};
  L0DB* _l0_b_db{nullptr};
  uint64_t _id{0};

  // Which cursor is currently "active" (pointing to current position)
  enum class Active { L0, L0_INACTIVE, L1, BOTH, NONE } _active = Active::NONE;

  _LSMCursor(LSMDB* lsm_db) : _lsm_db(lsm_db) {
    _id = lsm_db->new_cursor_id();

    // Reference and create cursors for both L0 databases
    _l0_a_db = lsm_db->_l0_a.get();
    _l0_b_db = lsm_db->_l0_b.get();
    lsm_db->ref_l0(_l0_a_db);
    lsm_db->ref_l0(_l0_b_db);

    _l0_a_cursor = _l0_a_db->create_cursor();
    _l0_b_cursor = _l0_b_db->create_cursor();
    _l1_cursor = lsm_db->l1()->create_cursor();

    _refresh_l0_pointers();
  }

  ~_LSMCursor() {
    _lsm_db->unref_l0(_l0_a_db);
    _lsm_db->unref_l0(_l0_b_db);
  }

  bool is_valid() const {
    return _l0_active->is_valid() || _l1_cursor->is_valid() ||
           (_lsm_db->_header->merge_in_progress.load() && _l0_inactive->is_valid());
  }

  Slice key() const {
    switch (_active) {
      case Active::L1:  // Most common case - majority of data is in L1
        return _l1_cursor->key();
      case Active::L0_INACTIVE:
        return _l0_inactive->key();
      case Active::L0:
      case Active::BOTH:
        return _l0_active->key();
      default:
        return Slice();
    }
  }

  Slice value() const {
    switch (_active) {
      case Active::L1:  // Most common case - majority of data is in L1
        return _l1_cursor->value();
      case Active::L0_INACTIVE:
        return _l0_inactive->value();
      case Active::L0:
      case Active::BOTH:  // L0 takes precedence for overwrites
        return _l0_active->value();
      default:
        return Slice();
    }
  }

  void find(const Slice& key) {
    _l0_active->find(key);
    _l0_inactive->find(key);
    _l1_cursor->find(key);
    _update_active();
  }

  void first() {
    _l0_active->first();
    _l0_inactive->first();
    _l1_cursor->first();
    _update_active();
  }

  void last() {
    _l0_active->last();
    _l0_inactive->last();
    _l1_cursor->last();
    _update_active_reverse();
  }

  void next() {
    // Advance whichever cursor(s) are at current position
    switch (_active) {
      case Active::L0:
        _l0_active->next();
        break;
      case Active::L0_INACTIVE:
        _l0_inactive->next();
        break;
      case Active::L1:
        _l1_cursor->next();
        break;
      case Active::BOTH:
        _l0_active->next();
        if (_lsm_db->_header->merge_in_progress.load() && _l0_inactive->is_valid() &&
            _l0_inactive->key() == _l0_active->key()) {
          _l0_inactive->next();
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
        _l0_active->prev();
        break;
      case Active::L0_INACTIVE:
        _l0_inactive->prev();
        break;
      case Active::L1:
        _l1_cursor->prev();
        break;
      case Active::BOTH:
        _l0_active->prev();
        if (_lsm_db->_header->merge_in_progress.load() && _l0_inactive->is_valid() &&
            _l0_inactive->key() == _l0_active->key()) {
          _l0_inactive->prev();
        }
        _l1_cursor->prev();
        break;
      default:
        return;
    }
    _update_active_reverse();
  }

  // Write operations go to active L0
  void* _reserve(size_t size) {
    // Need to find position in L0 if not already there
    if (_active == Active::L1 || _active == Active::NONE) {
      if (_l1_cursor->is_valid()) {
        _l0_active->find(_l1_cursor->key());
      }
    }
    return _l0_active->reserve(size);
  }

  void value(const Slice& val) {
    void* space = _reserve(val.size());
    memcpy(space, val.data(), val.size());
    _lsm_db->flush();

    // Update L0 size estimate and trigger merge if needed
    _lsm_db->_header->l0_size.fetch_add(val.size());
    if (_lsm_db->should_merge()) {
      bool zombified = _lsm_db->schedule_merge();
      if (zombified) {
        // Future active L0 was zombified - reconnect to new L0
        _reconnect_active_l0();
      }
      // Always refresh active/inactive pointers after L0 switch
      _refresh_l0_pointers();
    }
  }

  // Reconnect to the new active L0 after zombification
  void _reconnect_active_l0() {
    uint8_t active = _lsm_db->_header->active_l0.load();
    
    if (active == 0) {
      // A is now active - it was replaced
      L0DB* new_a = _lsm_db->_l0_a.get();
      _lsm_db->unref_l0(_l0_a_db);
      _l0_a_db = new_a;
      _lsm_db->ref_l0(_l0_a_db);
      _l0_a_cursor = _l0_a_db->create_cursor();
    } else {
      // B is now active - it was replaced
      L0DB* new_b = _lsm_db->_l0_b.get();
      _lsm_db->unref_l0(_l0_b_db);
      _l0_b_db = new_b;
      _lsm_db->ref_l0(_l0_b_db);
      _l0_b_cursor = _l0_b_db->create_cursor();
    }
  }

  // Refresh active/inactive pointers based on current active_l0 flag
  void _refresh_l0_pointers() {
    if (_lsm_db->_header->active_l0.load() == 0) {
      _l0_active = _l0_a_cursor.get();
      _l0_inactive = _l0_b_cursor.get();
    } else {
      _l0_active = _l0_b_cursor.get();
      _l0_inactive = _l0_a_cursor.get();
    }
  }

  void remove() {
    // For LSM, we could use tombstones, but for now just remove from L0
    // if present, and mark as deleted somehow
    if (_l0_active->is_valid()) {
      _l0_active->remove();
    }
    // TODO: Handle tombstones for L1 deletes
  }

  bool start_transaction(bool non_blocking = false) {
    return _l0_active->start_transaction(non_blocking);
  }

  tid_t prepare_commit(bool sync = false) {
    return _l0_active->prepare_commit(sync);
  }

  bool commit(bool sync = false) { return _l0_active->commit(sync); }

  bool rollback() { return _l0_active->rollback(); }

  // Determine which cursor is "active" based on key comparison
  // Priority: L0 (active) > L0 (inactive) > L1
  void _update_active() {
    bool merge_in_progress = _lsm_db->_header->merge_in_progress.load();
    bool l0_valid = _l0_active->is_valid();
    bool l0_inactive_valid = merge_in_progress && _l0_inactive->is_valid();
    bool l1_valid = _l1_cursor->is_valid();

    if (!l0_valid && !l0_inactive_valid && !l1_valid) {
      _active = Active::NONE;
      return;
    }

    // Find the smallest key among all valid cursors
    Slice min_key;
    Active min_cursor = Active::NONE;

    if (l0_valid) {
      min_key = _l0_active->key();
      min_cursor = Active::L0;
    }

    if (l0_inactive_valid) {
      Slice key = _l0_inactive->key();
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
    if (l0_valid && _l0_active->key().compare(min_key) == 0) {
      _active = Active::L0;  // L0 active always wins for equal keys
    } else if (l0_inactive_valid && _l0_inactive->key().compare(min_key) == 0) {
      _active = Active::L0_INACTIVE;
    } else {
      _active = min_cursor;
    }
  }

  // Same as _update_active but for reverse iteration
  void _update_active_reverse() {
    bool merge_in_progress = _lsm_db->_header->merge_in_progress.load();
    bool l0_valid = _l0_active->is_valid();
    bool l0_inactive_valid = merge_in_progress && _l0_inactive->is_valid();
    bool l1_valid = _l1_cursor->is_valid();

    if (!l0_valid && !l0_inactive_valid && !l1_valid) {
      _active = Active::NONE;
      return;
    }

    // Find the largest key among all valid cursors
    Slice max_key;
    Active max_cursor = Active::NONE;

    if (l0_valid) {
      max_key = _l0_active->key();
      max_cursor = Active::L0;
    }

    if (l0_inactive_valid) {
      Slice key = _l0_inactive->key();
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
    if (l0_valid && _l0_active->key().compare(max_key) == 0) {
      _active = Active::L0;  // L0 active always wins for equal keys
    } else if (l0_inactive_valid && _l0_inactive->key().compare(max_key) == 0) {
      _active = Active::L0_INACTIVE;
    } else {
      _active = max_cursor;
    }
  }

  // Debug: dump all three tries to yaml files
  void dump_debug() {
    // Dump L0 A
    {
      std::ofstream out("/tmp/l0_a.yaml");
      auto root = _l0_a_db->txn()->root;
      _Dumper(*_l0_a_db, root, false).dump(out);
    }
    // Dump L0 B
    {
      std::ofstream out("/tmp/l0_b.yaml");
      auto root = _l0_b_db->txn()->root;
      _Dumper(*_l0_b_db, root, false).dump(out);
    }
    // Dump L1
    {
      std::ofstream out("/tmp/l1.yaml");
      auto root = _lsm_db->l1()->txn()->root;
      _Dumper(*_lsm_db->l1(), root, false).dump(out);
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_LSM_HPP
