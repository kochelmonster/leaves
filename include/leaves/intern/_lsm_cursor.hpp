#ifndef _LEAVES__LSM_CURSOR_HPP
#define _LEAVES__LSM_CURSOR_HPP

#include <memory>
#include <vector>

#include "_cursor.hpp"
#include "_lsm_db.hpp"

namespace leaves {

// LSM-aware cursor with multi-source search and tombstone handling
template <typename DB_, typename Traits_>
struct _LSMCursor {
  typedef DB_ DB;
  typedef Traits_ Traits;
  typedef _Cursor<DB_, typename DB::BaseCursorTraits> BaseCursor;
  typedef typename DB::SegmentType Segment;
  typedef typename Segment::Traits SegmentTraits;
  typedef _Cursor<Segment, SegmentTraits> SegmentCursor;
  using txn_ptr = typename DB::txn_ptr;
  using db_ptr = typename Traits::db_ptr;
  static_assert(Traits_::MAX_KEY_SIZE <= DB_::Transaction::SEGMENT_SIZE / 2,
                "Cursor key size exceeds segment size");

  db_ptr _db;

  // Persistent storage cursor
  BaseCursor _base_cursor;

  // Per-segment cursors (newest to oldest, reverse order from segment_head
  // list) Index 0 is always the current transaction's segment (for writes)
  std::vector<std::unique_ptr<SegmentCursor>> _segment_cursors;

  // Active cursor for delegation
  // nullptr = use _base_cursor (persistent storage)
  // non-null = points to segment cursor from _segment_cursors
  SegmentCursor* _active_cursor{nullptr};
  bool _needs_cursor_sync{false};  // True after find(), cleared by next/prev

  _LSMCursor(db_ptr db) : _db(db), _base_cursor(_db.get()) {
    _acquire_segment_refs();
  }

  ~_LSMCursor() { _release_segment_refs(); }

  // Override find to search all sources
  void find(const Slice& key) {
    _check_segment_refs_validity();

    _needs_cursor_sync = true;

    // Search segment cursors from newest to oldest (reverse order)
    for (auto& cursor : _segment_cursors) {
      cursor->find(key);
      if (cursor->is_valid()) {
        _active_cursor = cursor.get();
        return;
      }
    }

    // Not found in segments, search persistent storage
    _base_cursor.find(key);
    _active_cursor = nullptr;  
  }

  // Override is_valid to use active cursor
  bool is_valid() const {
    return _active_cursor ? _active_cursor->is_valid() : _base_cursor.is_valid();
  }

  Slice key() const {
    return _active_cursor ? _active_cursor->key() : _base_cursor.key();
  }

  Slice value() const {
    return _active_cursor ? _active_cursor->value() : _base_cursor.value();
  }

  // Override navigation methods
  void first() {
    _check_segment_refs_validity();
    _needs_cursor_sync = false;

    if (_segment_cursors.empty()) {
      _base_cursor.first();
      return;
    }

    for (auto& cursor : _segment_cursors) {
      cursor->first();
    }
    _base_cursor.first();

    _find_min_cursor();
  }

  void last() {
    _check_segment_refs_validity();
    _needs_cursor_sync = false;

    if (_segment_cursors.empty()) {
      _base_cursor.last();
      return;
    }

    for (auto& cursor : _segment_cursors) {
      cursor->last();
    }
    _base_cursor.last();

    _find_max_cursor();
  }

  void next() {
    _check_segment_refs_validity();

    if (!is_valid()) return;

    if (_segment_cursors.empty()) {
      _base_cursor.next();
      return;
    }

    _sync_cursors_after_find();

    if (_active_cursor) {
      _active_cursor->next();
    } else {
      _base_cursor.next();
    }

    _find_min_cursor();
  }

  void prev() {
    _check_segment_refs_validity();

    if (!is_valid()) return;

    if (_segment_cursors.empty()) {
      _base_cursor.prev();
      return;
    }

    _sync_cursors_after_find();

    if (_active_cursor) {
      _active_cursor->prev();
    } else {
      _base_cursor.prev();
    }

    _find_max_cursor();
  }

  void remove() {
    // LSM cursors never delete - they insert tombstones instead
    if (!is_valid()) throw NoValidPosition();

    [[maybe_unused]] bool r = start_transaction();
    assert(r);

    auto& segment_cursor = _segment_cursors[0];
    if (_active_cursor != segment_cursor.get()) {
      segment_cursor->find(key());
    }

    segment_cursor->insert(key(), Slice());
    auto* leaf = segment_cursor->leaf();
    assert(leaf);
    leaf->set_tombstone();
  }

  void* reserve(size_t size) {
    [[maybe_unused]] bool r = start_transaction();
    assert(r);

    while (true) {
      auto& segment_cursor = _segment_cursors[0];
      if (_active_cursor != segment_cursor.get()) {
        segment_cursor->find(key());
        _active_cursor = segment_cursor.get();
      }

      void* result = _active_cursor->reserve(size);
      if (result) return result;

      // Segment overflow - create new segment and retry
      _db->add_segment(_db->_wtxn);
      _add_current_segment();
      // now reserve will succeed
    }
  }

  void value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);
    memcpy(space, value.data(), value.size());
  }

  // Override start_transaction to add current segment
  bool start_transaction(bool non_blocking = false) {
    if (_db->start_transaction(_base_cursor._id, non_blocking)) {
      _add_current_segment();
      return true;
    }
    return false;
  }

  tid_t prepare_commit(bool sync = false) {
    return _db->prepare_commit(_base_cursor._id, sync);
  }

  bool commit(bool sync = false) {
    return _db->commit(_base_cursor._id, sync);
  }

  // Helper methods
  void _check_segment_refs_validity() {
    // Only check if cursor has segment references
    if (_segment_cursors.empty()) return;

    auto& cursor_txn =
        static_cast<typename DB::Transaction&>(*_base_cursor._txn);

    // If cursor's transaction is COMMITTED, all its segments are merged -
    // release them
    if (cursor_txn.merge_phase == COMMITTED) {
      std::string saved_key;
      if (is_valid()) {
        saved_key =
            (_active_cursor ? _active_cursor->key() : _base_cursor.key())
                .string();
      }
      _release_segment_refs();
      _base_cursor.stack.clear();
      if (!saved_key.empty()) {
        find(saved_key);  // Reposition to persistent storage
      }
    }
  }

  void _acquire_segment_refs() {
    _release_segment_refs();

    if (!_base_cursor._txn) return;

    auto& cursor_txn =
        static_cast<typename DB::Transaction&>(*_base_cursor._txn);
    tid_t cursor_txn_id = cursor_txn.txn_id;

    // Collect segments from all COMMITTING transactions that are older than
    // cursor's transaction
    _base_cursor._db->iter_transactions([&](txn_ptr txn) -> bool {
      auto& lsm_txn = static_cast<typename DB::Transaction&>(*txn);

      // Stop when we reach transactions newer than cursor's transaction
      if (lsm_txn.txn_id > cursor_txn_id) {
        return true;  // Break iteration
      }

      // Only collect segments from COMMITTING transactions
      if (lsm_txn.merge_phase == COMMITTING) {
        // Collect all segments from this transaction (oldest first in linked
        // list)
        lsm_txn.iter_segments(
            _db.get(), [&](auto segment, offset_t seg_offset) -> bool {
              segment->acquire();

              // Create cursor for this segment (segment is a
              // Segment pointer)
              auto cursor = std::make_unique<SegmentCursor>(segment);
              _segment_cursors.push_back(std::move(cursor));

              return false;  // Continue to next segment
            });
      }

      return false;  // Continue to next transaction
    });

    // Reverse to get newest first (search priority: newest data checked first)
    std::reverse(_segment_cursors.begin(), _segment_cursors.end());
  }

  void _release_segment_refs() {
    if (_segment_cursors.empty()) return;

    // Release all segment cursors
    for (auto& cursor : _segment_cursors) {
      // Cursor's _db is a Segment::ptr, dereference to get raw pointer
      Segment* segment = &*cursor->_db;
      segment->release();
    }
    _segment_cursors.clear();
    _active_cursor = nullptr;
  }

  void _add_current_segment() {
    if (!_base_cursor._txn) return;

    auto& lsm_txn = _db->_wtxn;

    if (lsm_txn->current_segment) {
      typename Segment::ptr segment =
          _db->resolve(lsm_txn->current_segment, WRITE);
      segment->acquire();
      // Insert at index 0 (newest segment for writes)
      auto cursor = std::make_unique<SegmentCursor>(segment);
      // non transaction cursors need explicitly set transactions
      cursor->_txn = segment->txn();
      _segment_cursors.insert(_segment_cursors.begin(), std::move(cursor));
    }
  }

  void _sync_cursors_after_find() {
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

  // Find cursor with minimum key (for forward iteration)
  void _find_min_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;

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
          return nullptr;
        }
        cursor->next();
      }
      return nullptr;
    };

    // Scan all segment cursors
    for (auto& cursor : _segment_cursors) {
      if (auto found = get_best(cursor.get())) {
        best = found;
      }
    }

    if (auto found = get_best(&_base_cursor)) {
      best = nullptr;
    }

    _active_cursor = best;

    // If we found a key, skip all duplicates
    if (has_best) {
      Slice current_key = best ? best->key() : _base_cursor.key();

      // Advance all other cursors at this key
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

  // Find cursor with maximum key (for backward iteration)
  void _find_max_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;

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
          return nullptr;
        }
        cursor->prev();
      }
      return nullptr;
    };

    // Scan all segment cursors
    for (auto& cursor : _segment_cursors) {
      if (auto found = get_best(cursor.get())) {
        best = found;
      }
    }

    if (auto found = get_best(&_base_cursor)) {
      best = nullptr;
    }

    _active_cursor = best;

    // If we found a key, skip all duplicates
    if (has_best) {
      Slice current_key = best ? best->key() : _base_cursor.key();

      // Move back all other cursors at this key
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
};

}  // namespace leaves

#endif  // _LEAVES__LSM_CURSOR_HPP
