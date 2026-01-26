/*
Todos: 
  - Zombie count ist schlecht statt dessen L0DB mit erweiterten header für ref count.
  - Löschen -> node zombies
  - big values -> beide l0 sollen mit dem gleichen root arbeiten
  - was ist mit rollback? Kein l1 commit nach merge. 
       Eine Haupttransaction, die die roots für l0 hält
*/

#ifndef _LEAVES_LSM_HPP
#define _LEAVES_LSM_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

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
 * - Zombie L0s: when L0 can't be reset due to refs, it becomes a zombie
 */

/**
 * @brief Persistent linked list entry for zombie L0 databases
 */
struct ZombieL0Entry {
  offset_t db_header;  // The zombie L0 database header
  offset_t next;       // Next zombie in linked list (0 = end)
};

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

  // Head of zombie L0 linked list (for crash recovery)
  offset_t zombie_list_head;

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

  // Zombie L0 tracking (in-memory) - use uint64_t keys for hash map compatibility
  std::shared_mutex _zombie_mutex;
  std::unordered_map<uint64_t, std::atomic<uint32_t>> _zombie_refs;
  std::unordered_map<uint64_t, std::unique_ptr<DB>> _zombie_dbs;

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
    _header->zombie_list_head = 0;  // Empty zombie list

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

  // Decrement reference count for a zombie L0
  // If refs drop to 0, schedule cleanup
  void unref_zombie(offset_t zombie_header) {
    uint64_t key = zombie_header._offset;
    std::shared_lock lock(_zombie_mutex);
    auto it = _zombie_refs.find(key);
    if (it != _zombie_refs.end()) {
      if (it->second.fetch_sub(1) == 1) {
        // Refs dropped to 0, schedule cleanup
        lock.unlock();
        schedule_zombie_cleanup(zombie_header);
      }
    }
  }

  // Reference a zombie L0
  void ref_zombie(offset_t zombie_header) {
    uint64_t key = zombie_header._offset;
    std::shared_lock lock(_zombie_mutex);
    auto it = _zombie_refs.find(key);
    if (it != _zombie_refs.end()) {
      it->second.fetch_add(1);
    }
  }

  // Schedule cleanup of a zombie L0
  void schedule_zombie_cleanup(offset_t zombie_header) {
    _storage.submit_task([this, zombie_header]() {
      cleanup_zombie(zombie_header);
    });
  }

  // Clean up a zombie L0: remove from list, free memory
  void cleanup_zombie(offset_t zombie_header) {
    uint64_t key = zombie_header._offset;
    std::unique_lock lock(_zombie_mutex);
    
    // Remove from in-memory tracking
    _zombie_refs.erase(key);
    auto db_it = _zombie_dbs.find(key);
    if (db_it != _zombie_dbs.end()) {
      db_it->second->return_areas();
      _zombie_dbs.erase(db_it);
    }
    
    // Remove from persistent linked list
    remove_zombie_from_list(zombie_header);
  }

  // Remove a zombie from the persistent linked list
  void remove_zombie_from_list(offset_t zombie_header) {
    offset_t* prev_ptr = &_header->zombie_list_head;
    offset_t current = _header->zombie_list_head;
    
    while (current != 0) {
      auto raw_ptr = _storage.resolve(&current, READ);
      ZombieL0Entry* entry = reinterpret_cast<ZombieL0Entry*>(&*raw_ptr);
      if (entry->db_header == zombie_header) {
        // Found it - unlink
        *prev_ptr = entry->next;
        _storage.make_dirty(_header);
        // Free the zombie entry itself
        // (the DB areas were already freed by return_areas)
        return;
      }
      prev_ptr = &entry->next;
      current = entry->next;
    }
  }

  // Create a zombie from an L0 that can't be reset, atomically replacing it with a fresh L0
  offset_t zombify_l0(uint8_t which) {
    std::unique_lock lock(_zombie_mutex);
    
    // Get the DB header offset BEFORE we change it
    offset_t db_header = which == 0 ? _header->l0_a_header : _header->l0_b_header;
    uint64_t key = db_header._offset;
    
    // Create zombie entry in storage
    auto area = _storage.alloc_single_area();
    offset_t entry_offset = area->content_offset();
    auto raw_ptr = _storage.resolve(&entry_offset, READ);
    ZombieL0Entry* entry = reinterpret_cast<ZombieL0Entry*>(&*raw_ptr);
    entry->db_header = db_header;
    entry->next = _header->zombie_list_head;
    _header->zombie_list_head = entry_offset;
    _storage.make_dirty(_header);
    
    // Get current ref count - this is how many cursors are using this L0
    // These cursors will call unref and we need to track when it hits 0
    uint32_t refs = which == 0 ? _header->l0_a_refs.load() 
                                : _header->l0_b_refs.load();
    _zombie_refs[key].store(refs);
    
    // Create new L0 BEFORE moving old one to ensure no null window
    auto new_area = _storage.alloc_single_area();
    offset_t new_header = new_area->content_offset();
    auto new_db = std::make_unique<DB>(_storage, &new_header, _index);
    
    // Atomically swap: move old to zombie, install new
    if (which == 0) {
      _zombie_dbs[key] = std::move(_l0_a);
      _l0_a = std::move(new_db);
      _header->l0_a_header = new_header;
      _header->l0_a_refs.store(0);
    } else {
      _zombie_dbs[key] = std::move(_l0_b);
      _l0_b = std::move(new_db);
      _header->l0_b_header = new_header;
      _header->l0_b_refs.store(0);
    }
    _storage.make_dirty(_header);
    
    return db_header;
  }

  // Get a zombie DB for cursor creation
  DB* get_zombie_db(offset_t zombie_header) {
    uint64_t key = zombie_header._offset;
    std::shared_lock lock(_zombie_mutex);
    auto it = _zombie_dbs.find(key);
    return it != _zombie_dbs.end() ? it->second.get() : nullptr;
  }

  // Prepare inactive L0 for reuse: either reset it or zombify it
  // Returns true if a zombie was created
  bool prepare_inactive_l0() {
    uint8_t active = _header->active_l0.load();
    uint8_t inactive = active == 0 ? 1 : 0;
    
    // Check if inactive L0 has any refs
    uint32_t refs = inactive == 0 ? _header->l0_a_refs.load() 
                                   : _header->l0_b_refs.load();
    
    if (refs == 0) {
      // No refs - we can reset it directly
      if (inactive == 0) {
        _l0_a->reset(&_header->l0_a_header);
      } else {
        _l0_b->reset(&_header->l0_b_header);
      }
      _storage.make_dirty(_header);
      return false;
    } else {
      // Has refs - zombify it (which also creates fresh L0 atomically)
      zombify_l0(inactive);
      return true;
    }
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

    // Get the L0 that we're merging FROM (it's now inactive)
    // We need to get the actual DB pointer - it could be the regular L0
    // or if it's already been zombified, we need to merge from the zombie
    DB* merge_source_l0;
    uint8_t active = _header->active_l0.load();
    if (active == 0) {
      merge_source_l0 = _l0_b.get();  // B is inactive
    } else {
      merge_source_l0 = _l0_a.get();  // A is inactive
    }

    // Merge L0 into L1
    {
      auto l0_cursor = merge_source_l0->create_cursor();
      auto l1_cursor = _l1->create_cursor();

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

    // Merge is complete. Now prepare the inactive L0 for next use.
    // If it has refs, zombify it and create fresh. Otherwise reset.
    prepare_inactive_l0();

    _header->merge_in_progress.store(false);
    _storage.make_dirty(_header);
  }

  void sanitize() {
    _l0_a->sanitize();
    _l0_b->sanitize();
    _l1->sanitize();
    new (&_header->merge_lock) Mutex();
    _header->merge_in_progress.store(false);

    // Clean up zombie list - after crash, all zombie refs are gone
    // so we can reclaim all zombies
    reclaim_all_zombies();

    // Reschedule merge if L0 needs compaction
    if (should_merge()) {
      schedule_merge();
    }
  }

  // Reclaim all zombies after a crash recovery
  void reclaim_all_zombies() {
    while (_header->zombie_list_head != 0) {
      offset_t entry_offset = _header->zombie_list_head;
      auto raw_ptr = _storage.resolve(&entry_offset, READ);
      ZombieL0Entry* entry = reinterpret_cast<ZombieL0Entry*>(&*raw_ptr);
      
      // Load the zombie DB to get its areas
      DB zombie_db(_storage, entry->db_header, _index);
      zombie_db.return_areas();
      
      // Remove from list
      _header->zombie_list_head = entry->next;
      _storage.make_dirty(_header);
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
  uint8_t _l0_which;  // Which L0 buffer this cursor is using (0 or 1)
  uint8_t _l0_inactive_which{0};  // Which L0 buffer the inactive cursor uses
  offset_e _l0_header_offset{0};  // Header offset of the L0 we're using
  offset_e _l0_inactive_header_offset{0};  // Header offset of inactive L0
  bool _has_inactive_l0{false};  // Whether we have an inactive L0 cursor
  uint64_t _id{0};

  // Which cursor is currently "active" (pointing to current position)
  enum class Active { L0, L0_INACTIVE, L1, BOTH, NONE } _active = Active::NONE;

  _LSMCursor(LSMDB* lsm_db)
      : _lsm_db(lsm_db), _l0_which(lsm_db->_header->active_l0.load()) {
    _id = lsm_db->new_cursor_id();

    // Reference the L0 we're using
    lsm_db->ref_l0(_l0_which);
    _l0_header_offset = _l0_which == 0 ? lsm_db->_header->l0_a_header 
                                        : lsm_db->_header->l0_b_header;

    // Create cursors for both levels
    DB* l0 = _l0_which == 0 ? lsm_db->_l0_a.get() : lsm_db->_l0_b.get();
    _l0_cursor = l0->create_cursor();
    _l1_cursor = lsm_db->l1()->create_cursor();

    // If merge is in progress, also create cursor for inactive L0
    // We ref-count it so the merge knows not to reset it until we're done
    if (lsm_db->_header->merge_in_progress.load()) {
      _l0_inactive_which = _l0_which == 0 ? 1 : 0;
      lsm_db->ref_l0(_l0_inactive_which);
      _l0_inactive_header_offset = _l0_inactive_which == 0 
          ? lsm_db->_header->l0_a_header 
          : lsm_db->_header->l0_b_header;
      DB* inactive_l0 = _l0_inactive_which == 0 ? lsm_db->_l0_a.get() : lsm_db->_l0_b.get();
      _l0_inactive_cursor = inactive_l0->create_cursor();
      _has_inactive_l0 = true;
    }
  }

  ~_LSMCursor() {
    // Release L0 reference
    // Check if it became a zombie - compare header offsets
    if (_lsm_db->get_zombie_db(_l0_header_offset)) {
      _lsm_db->unref_zombie(_l0_header_offset);
    } else {
      _lsm_db->unref_l0(_l0_which);
    }
    
    // Release inactive L0 reference if we have one
    if (_has_inactive_l0) {
      if (_lsm_db->get_zombie_db(_l0_inactive_header_offset)) {
        _lsm_db->unref_zombie(_l0_inactive_header_offset);
      } else {
        _lsm_db->unref_l0(_l0_inactive_which);
      }
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
