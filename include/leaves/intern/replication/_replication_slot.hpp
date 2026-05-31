#ifndef _LEAVES__REPLICATION_SLOT_HPP
#define _LEAVES__REPLICATION_SLOT_HPP

#include <atomic>
#include <cstdint>

namespace leaves {

// Replication Slot — crash-safe tracking of pre-merge multi-areas
//
// The FSM claims one slot in _header->replication_slots[].  The slot
// holds the offset of the multi-area currently being filled for big
// values.  On merge, the area is handed to the transaction and the slot
// is cleared.  On crash, sanitize() returns all non-zero slots to the pool.

template <typename DB>
struct _ReplicationSlot {
  DB* _db;
  int16_t _slot;  // -1 = no slot claimed

  _ReplicationSlot(DB* db) : _db(db), _slot(-1) {}

  ~_ReplicationSlot() { release(); }

  _ReplicationSlot(const _ReplicationSlot&) = delete;
  _ReplicationSlot& operator=(const _ReplicationSlot&) = delete;

  // Claim a replication slot in _header->replication_slots[].
  // Uses atomic CAS (0 → sentinel) to claim without file_lock().
  void claim() {
    constexpr auto N = DB::Header::MAX_REPLICATION_SLOTS;
    constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
    for (uint16_t i = 0; i < N; ++i) {
      auto& slot = _db->_header->replication_slots[i];
      uint64_t expected = 0;
      if (std::atomic_ref<uint64_t>(slot._offset)
              .compare_exchange_strong(expected, SENTINEL,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        _slot = static_cast<int16_t>(i);
        return;
      }
    }
    // All slots occupied — proceed without crash-safety tracking.
    // This is a soft failure: the session will work but a crash could
    // leak one multi-area.
    _slot = -1;
  }

  // Store the multi-area offset in the claimed slot and flush.
  // Sole-owner operation — no lock needed; atomic store for visibility.
  void track_area(typename DB::Storage::area_ptr area) {
    if (_slot < 0) return;

    auto& slot = _db->_header->replication_slots[_slot];
    offset_t off = _db->resolve(area);
    std::atomic_ref<uint64_t>(slot._offset)
        .store(off._offset, std::memory_order_release);
    _db->make_dirty(_db->_header);
    _db->flush();
  }

  // Reset slot to sentinel — area is now transaction-owned but keep the
  // slot claimed so another receiver cannot steal it between rounds.
  // On crash: _sanitize_replication_anchors treats SENTINEL as no-op;
  // the area is reclaimed by Base::sanitize() via return_areas_range.
  void clear_area() {
    if (_slot < 0) return;

    constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
    _db->_header->replication_slots[_slot] = SENTINEL;
    _db->make_dirty(_db->_header);
    _db->flush();
  }

  // Release the replication slot.  If it still holds a non-zero offset
  // (error path — area was never merged), return it to the pool first.
  // Sole-owner operation — no lock needed; atomic store for visibility.
  void release() {
    if (_slot < 0) return;

    constexpr uint64_t SENTINEL = DB::Header::REPLICATION_SLOT_SENTINEL;
    auto& slot = _db->_header->replication_slots[_slot];
    uint64_t raw =
        std::atomic_ref<uint64_t>(slot._offset).load(std::memory_order_acquire);
    if (raw && raw != SENTINEL) {
      offset_t off;
      off._offset = raw;
      _db->_storage.return_multi_areas(off, off);
    }
    if (raw) {
      std::atomic_ref<uint64_t>(slot._offset)
          .store(0, std::memory_order_release);
      _db->make_dirty(_db->_header);
      _db->flush();
    }
    _slot = -1;
  }

  int16_t slot_index() const { return _slot; }
  bool has_slot() const { return _slot >= 0; }
};

}  // namespace leaves

#endif  // _LEAVES__REPLICATION_SLOT_HPP
