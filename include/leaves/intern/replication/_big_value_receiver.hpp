/*
Replication-side receiver for large value chunks and deferred payloads.
*/
#ifndef _LEAVES__BIG_VALUE_RECEIVER_HPP
#define _LEAVES__BIG_VALUE_RECEIVER_HPP

#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>

#include "../core/_exception.hpp"
#include "../core/_util.hpp"
#include "../memory/_bigmemory.hpp"
#include "../memory/_memory.hpp"
#include "_replication_protocol.hpp"
#include "_replication_slot.hpp"

namespace leaves {

// Big Value Receiver — receives and stores big values during replication
//
// Manages the receiver-side state for big value streaming:
//   1. Allocates persistent storage on BIG_VALUE_START
//   2. Writes incoming data chunks on BIG_VALUE_DATA
//   3. Links the allocated area to the transaction during merge
//   4. Maps wire offsets → persistent offsets for the merge policy
//
// Error handling: methods return _Error (code + message). The FSM checks
// the return value and calls _transition_to_error if needed.

template <typename DB>
struct _BigValueReceiver {
  using Traits = typename DB::Traits;

  // Dummy struct for raw data pointers (same as in the FSM)
  struct Chunk {};
  using chunk_ptr = typename Traits::template Pointer<Chunk>;
  using offset_t = leaves::offset_t;

  static constexpr size_t MAX_PAGE_SIZE =
      Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
  static constexpr size_t AREA_SIZE = Traits::AREA_SIZE;
  static constexpr size_t FREE_KEY_SIZE = sizeof(_FreeKey);
  static constexpr size_t DEFAULT_MAX_SIZE = 256 * 1024 * 1024;  // 256MB

  // Error result from an operation
  struct _Error {
    ReplicationError code = ReplicationError::NONE;
    const char* message = nullptr;
    explicit operator bool() const { return code != ReplicationError::NONE; }
  };

  // Storage state
  std::unordered_map<uint64_t, offset_t> _offsets;
  typename DB::Storage::area_ptr _multi_area;
  std::vector<uint8_t> _tmp_area;
  bool _using_tmp_area;
  chunk_ptr _area;
  offset_t _area_offset;
  size_t _area_size;
  size_t _write_pos;
  uint32_t _expected_count;
  uint32_t _received_count;
  uint32_t _trie_count;  // Count of big value leaves found in received trie

  // Stream parsing state
  uint8_t _header_buf[sizeof(BigValueDataHeader)];
  size_t _header_pos;
  uint64_t _current_wire_offset;
  uint32_t _current_size;
  size_t _current_received;
  bool _parsing_header;

  size_t _max_size;

  _BigValueReceiver(size_t max_size = DEFAULT_MAX_SIZE)
      : _multi_area(nullptr),
        _tmp_area(AREA_SIZE),
        _using_tmp_area(false),
        _area(nullptr),
        _area_offset(0),
        _area_size(0),
        _write_pos(0),
        _expected_count(0),
        _received_count(0),
        _trie_count(0),
        _header_pos(0),
        _current_wire_offset(0),
        _current_size(0),
        _current_received(0),
        _parsing_header(true),
        _max_size(max_size) {}

  // Clear storage state (after merge). Does NOT reset trie_count.
  void clear() {
    _offsets.clear();
    _multi_area = nullptr;
    _using_tmp_area = false;
    _area = nullptr;
    _area_offset = 0;
    _area_size = 0;
    _write_pos = 0;
  }

  // Reset all state for a new replication session (called in begin()).
  void reset() {
    clear();
    _trie_count = 0;
  }

  // Process BIG_VALUE_START payload.
  // Allocates persistent storage and tracks it in the replication slot.
  _Error handle_start(DB* db, const Slice& payload,
                      _ReplicationSlot<DB>& slot) {
    if (payload.size() < sizeof(BigValueStartHeader)) {
      return {ReplicationError::INVALID_MESSAGE,
              "BIG_VALUE_START payload too small"};
    }

    const auto* hdr = (const BigValueStartHeader*)payload.data();

    _expected_count = hdr->count;
    _received_count = 0;
    uint64_t total_aligned_size = hdr->total_aligned_size;

    if (total_aligned_size > _max_size) {
      return {ReplicationError::RESOURCE_LIMIT,
              "Big value total size exceeds limit"};
    }

    uint64_t alloc_size_64 =
        ((total_aligned_size + AREA_SIZE - 1) / AREA_SIZE) * AREA_SIZE;
    if (alloc_size_64 > SIZE_MAX) {
      return {ReplicationError::RESOURCE_LIMIT,
              "Big value allocation overflow"};
    }
    size_t alloc_size = static_cast<size_t>(alloc_size_64);
    if (alloc_size <= AREA_SIZE) {
      // Small transfer: store data in temporary in-memory area and defer
      // allocation to merge-time copy.
      _using_tmp_area = true;
      _multi_area = nullptr;
      _area = nullptr;
      _area_offset = 0;
      _area_size = AREA_SIZE;
    } else {
      _using_tmp_area = false;
      try {
        _multi_area = db->_storage.alloc_multi_area(alloc_size);
      } catch (const std::exception& e) {
        return {ReplicationError::INTERNAL_ERROR, e.what()};
      }

      slot.track_area(_multi_area);

      _area_offset = _multi_area->content_offset();
      _area_size = _multi_area->end() - _area_offset;
      _area = db->template resolve<Chunk>(&_area_offset, WRITE);
    }
    _write_pos = 0;
    _offsets.clear();

    // Reset stream parsing state
    _header_pos = 0;
    _current_wire_offset = 0;
    _current_size = 0;
    _current_received = 0;
    _parsing_header = true;

    return {};
  }

  // Process BIG_VALUE_DATA payload.
  // Returns bytes_delta (for progress tracking) and all_received flag.
  _Error handle_data(const Slice& payload, size_t& bytes_delta,
                     bool& all_received) {
    char* base = _using_tmp_area ? (char*)_tmp_area.data() : (char*)_area;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(payload.data());
    const uint8_t* end = ptr + payload.size();

    bytes_delta = 0;
    all_received = false;

    while (ptr < end) {
      if (_parsing_header) {
        size_t header_needed = sizeof(BigValueDataHeader) - _header_pos;
        size_t header_available = end - ptr;
        size_t header_to_read = std::min(header_needed, header_available);

        std::memcpy(_header_buf + _header_pos, ptr, header_to_read);
        _header_pos += header_to_read;
        ptr += header_to_read;

        if (_header_pos < sizeof(BigValueDataHeader)) break;

        const auto* hdr = (const BigValueDataHeader*)_header_buf;
        _current_wire_offset = hdr->wire_chunk_offset;
        _current_size = hdr->value_size;
        _current_received = 0;
        _parsing_header = false;

        size_t chunk_size = FREE_KEY_SIZE + _current_size;
        size_t aligned_size = padding(chunk_size, MAX_PAGE_SIZE);

        size_t needed = _write_pos + aligned_size;
        if (needed > _area_size) {
          return {ReplicationError::INTERNAL_ERROR,
                  "Big value area overflow"};
        }

        char* chunk_ptr = base + _write_pos;
        offset_t header_offset = _area_offset + _write_pos;

        _FreeKey* header = (_FreeKey*)chunk_ptr;
        header->size = aligned_size;
        bool has_successor =
            (_received_count + 1) < _expected_count;
        header->offset = header_offset._offset | (has_successor ? 1 : 0);

        offset_t data_offset = header_offset + FREE_KEY_SIZE;
        _offsets[_current_wire_offset] = data_offset;
      }

      size_t data_needed = _current_size - _current_received;
      size_t data_available = end - ptr;
      size_t data_to_read = std::min(data_needed, data_available);

      if (data_to_read > 0) {
        char* chunk_ptr = base + _write_pos;
        std::memcpy(chunk_ptr + FREE_KEY_SIZE + _current_received, ptr,
                    data_to_read);
        _current_received += data_to_read;
        ptr += data_to_read;
        bytes_delta += data_to_read;
      }

      if (_current_received >= _current_size) {
        size_t chunk_size = FREE_KEY_SIZE + _current_size;
        _write_pos += padding(chunk_size, MAX_PAGE_SIZE);
        ++_received_count;
        _header_pos = 0;
        _parsing_header = true;
      }
    }

    all_received = (_received_count >= _expected_count);
    return {};
  }

  // Link the pre-allocated multi-area to the active transaction.
  // Must be called within a transaction before merging big values.
  void link_area(DB* db, _ReplicationSlot<DB>& slot) {
    if (!_multi_area) return;

    _multi_area->next = 0;
    offset_t area_off = db->resolve(_multi_area);
    if (db->_active_txn->area_list_tail_multi) {
      auto tail =
          db->template resolve<Area>(&db->_active_txn->area_list_tail_multi);
      tail->next = area_off;
      db->make_dirty(tail);
    } else {
      db->_header->area_list_head_multi = area_off;
      db->make_dirty(db->_header);
    }
    db->_active_txn->area_list_tail_multi = area_off;
    db->make_dirty(_multi_area);

    slot.clear_area();
  }

  // Accessors
  auto& offsets() { return _offsets; }
  const auto& offsets() const { return _offsets; }
  bool using_tmp_area() const { return _using_tmp_area; }
  const uint8_t* tmp_area_data() const { return _tmp_area.data(); }
  size_t area_size() const { return _area_size; }
  bool has_area() const { return _multi_area != nullptr; }
  uint32_t trie_count() const { return _trie_count; }
  void increment_trie_count() { ++_trie_count; }
  void reset_trie_count() { _trie_count = 0; }
};

}  // namespace leaves

#endif  // _LEAVES__BIG_VALUE_RECEIVER_HPP
