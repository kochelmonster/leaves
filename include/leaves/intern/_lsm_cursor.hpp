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
  typedef typename DB::BaseDB BaseDB;
  typedef _Cursor<BaseDB, typename BaseDB::ValueTraits> BaseCursor;
  typedef typename DB::SegmentType Segment;
  typedef typename Segment::Traits SegmentTraits;
  typedef _Cursor<Segment, SegmentTraits> SegmentCursor;
  using db_ptr = typename Traits::db_ptr;
  using lsm_txn_ptr = std::shared_ptr<typename DB::LSMTransaction>;

  db_ptr _db;
  lsm_txn_ptr _lsm_txn;  // Current LSM transaction

  // Persistent storage cursor
  BaseCursor _base_cursor;

  // Per-segment cursors (newest to oldest)
  // Index 0 is always the current transaction's segment (for writes)
  std::vector<std::unique_ptr<SegmentCursor>> _segment_cursors;

  // Active cursor for delegation
  SegmentCursor* _active_cursor{nullptr};
  bool _needs_cursor_sync{false};

  _LSMCursor(db_ptr db) : _db(db), _base_cursor(_db->_base_db.get()) {}

  ~_LSMCursor() { _release_segment_refs(); }

  // Find searches all sources
  void find(const Slice& key) {
    _needs_cursor_sync = true;

    // Search segment cursors from newest to oldest
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

  bool is_valid() const {
    return _active_cursor ? _active_cursor->is_valid() : _base_cursor.is_valid();
  }

  Slice key() const {
    return _active_cursor ? _active_cursor->key() : _base_cursor.key();
  }

  Slice value() const {
    return _active_cursor ? _active_cursor->value() : _base_cursor.value();
  }

  void first() {
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
    if (!is_valid()) throw NoValidPosition();

    [[maybe_unused]] bool r = start_transaction();
    assert(r);

    auto& segment_cursor = _segment_cursors[0];
    if (_active_cursor != segment_cursor.get()) {
      segment_cursor->find(key());
    }

    segment_cursor->value(Slice());  // Insert tombstone (empty value)
    auto* leaf = segment_cursor->stack.back().leaf();
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
      _db->add_segment(_lsm_txn);
      _add_current_segment();
    }
  }

  void value(const Slice& value) {
    void* space = reserve(value.size());
    assert(space);
    memcpy(space, value.data(), value.size());
  }

  bool start_transaction(bool non_blocking = false) {
    if (!_lsm_txn || _lsm_txn->merge_phase != WRITING) {
      _lsm_txn = _db->start_transaction();
      _add_current_segment();
      return true;
    }
    return true;  // Already have active transaction
  }

  bool commit(bool sync = false) {
    if (!_lsm_txn) return false;
    _db->commit_transaction(_lsm_txn, sync);
    _lsm_txn.reset();
    _release_segment_refs();
    return true;
  }

  bool rollback() {
    if (!_lsm_txn) return false;
    _db->rollback_transaction(_lsm_txn);
    _lsm_txn.reset();
    _release_segment_refs();
    return true;
  }

  // Helper methods
  void _release_segment_refs() {
    if (_segment_cursors.empty()) return;

    for (auto& cursor : _segment_cursors) {
      Segment* segment = cursor->_db;
      segment->release();
    }
    _segment_cursors.clear();
    _active_cursor = nullptr;
  }

  void _add_current_segment() {
    if (!_lsm_txn || !_lsm_txn->current_segment) return;

    typename Segment::ptr segment =
        _db->_base_db->template resolve<Segment>(_lsm_txn->current_segment, WRITE);
    segment->acquire();
    
    auto cursor = std::make_unique<SegmentCursor>(segment);
    cursor->_txn = segment->txn();
    _segment_cursors.insert(_segment_cursors.begin(), std::move(cursor));
  }

  void _sync_cursors_after_find() {
    if (!_needs_cursor_sync) return;

    _needs_cursor_sync = false;
    Slice current_key = key();

    for (auto& cursor : _segment_cursors) {
      if (cursor.get() != _active_cursor) {
        cursor->find(current_key);
      }
    }

    if (_active_cursor) {
      _base_cursor.find(current_key);
    }
  }

  void _find_min_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;

    auto get_best = [&](auto* cursor) -> decltype(cursor) {
      while (cursor->is_valid()) {
        auto& transition = cursor->stack.back();
        if (transition.is_leaf()) {
          auto* leaf = transition.leaf();
          if (!leaf || !leaf->is_tombstone()) {
            Slice key = cursor->key();
            if (!has_best || key < best_key) {
              best_key = key;
              has_best = true;
              return cursor;
            }
            return nullptr;
          }
        }
        cursor->next();
      }
      return nullptr;
    };

    for (auto& cursor : _segment_cursors) {
      if (auto found = get_best(cursor.get())) {
        best = found;
      }
    }

    if (auto found = get_best(&_base_cursor)) {
      best = nullptr;
    }

    _active_cursor = best;

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

  void _find_max_cursor() {
    SegmentCursor* best = nullptr;
    Slice best_key;
    bool has_best = false;

    auto get_best = [&](auto* cursor) -> decltype(cursor) {
      while (cursor->is_valid()) {
        auto& transition = cursor->stack.back();
        if (transition.is_leaf()) {
          auto* leaf = transition.leaf();
          if (!leaf || !leaf->is_tombstone()) {
            Slice key = cursor->key();
            if (!has_best || key > best_key) {
              best_key = key;
              has_best = true;
              return cursor;
            }
            return nullptr;
          }
        }
        cursor->prev();
      }
      return nullptr;
    };

    for (auto& cursor : _segment_cursors) {
      if (auto found = get_best(cursor.get())) {
        best = found;
      }
    }

    if (auto found = get_best(&_base_cursor)) {
      best = nullptr;
    }

    _active_cursor = best;

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
};

}  // namespace leaves

#endif  // _LEAVES__LSM_CURSOR_HPP
