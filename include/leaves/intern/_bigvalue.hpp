#ifndef _LEAVES_IBIGVALUE_HPP
#define _LEAVES_IBIGVALUE_HPP

// #define DEBUG_MEM
#ifdef DEBUG_MEM
#include <sstream>
#endif

#include "_util.hpp"

namespace leaves {

template <typename Traits>
struct BigMemory {
  using offset_e = typename Traits::offset_e;
  using uint64_e = typename Traits::uint64_e;

  struct BigSizeKey {
    boost::endian::big_uint64_t first;
    uint64_t second;
  };

  struct BigValue {
    uint64_e value_size;
    offset_e offset;
    size_t size() const { return value_size; }
  };


  uint16_tt modify_size(uint64_t size) {
    return size | BIG_VALUE_FLAG;
  }




  void _add_chunk(offset_t offset, size_t size) {
#ifdef DEBUG_MEM
    std::stringstream cstr;
    cstr << offset._offset << "-" << size;
#endif

    BigSizeKey bkey;
    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());

#ifdef DEBUG_MEM
    _mem_cursor.value(cstr.str());
#else
    _mem_cursor.value(Slice());
#endif

    bkey.first = size | SIZE_BIT;
    bkey.second = offset;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());
#ifdef DEBUG_MEM
    _mem_cursor.value(cstr.str());
#else
    _mem_cursor.value(Slice());
#endif
  }

  void _remove_chunk(offset_t offset, size_t size) {
    BigSizeKey bkey;
    bkey.first = size | SIZE_BIT;
    bkey.second = offset;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(_mem_cursor.is_valid());
    _mem_cursor.remove();

    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(_mem_cursor.is_valid());
    _mem_cursor.remove();
  }

  AreaSlice alloc(uint64_t size) {
    assert(_active_txn);

    size = padding(size, MAX_BLOCK_SIZE);
    uint32_t found_size;
    offset_t found_offset;

    // find from big memory storage
    BigSizeKey bkey;
    bkey.first = size | SIZE_BIT;
    bkey.second = 0;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    _mem_cursor.next();

    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      found_size = found->first & ~SIZE_BIT;
      found_offset = found->second;

      // remove the block from bit memory storage
      assert(found_size >= size);
      _mem_cursor.remove();

      // remove from offset list
      bkey.first = found_offset;
      bkey.second = found_size;
      _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
      assert(_mem_cursor.is_valid());
      _mem_cursor.remove();
    } else {
      // allocate new multi-area
      uint64_t psize = padding(size, AREA_SIZE);
      auto slice = alloc_multi_area(psize);
      found_offset = slice->content_offset();
      found_size = slice->end() - found_offset;
    }

    uint32_t delta = found_size - size;
    if (delta >= MAX_BLOCK_SIZE) {
      // enough space left -> reuse the rest
      _add_chunk(found_offset + size, delta);
      found_size -= delta;
    }

    return AreaSlice{found_offset, found_size};
  }

  void free(offset_e offset, size_t size) {
    assert(_active_txn);
    size = padding(size, MAX_BLOCK_SIZE);

    BigSizeKey bkey;
    bkey.first = offset;
    bkey.second = size;
    _mem_cursor.find(Slice(&bkey, sizeof(bkey)));
    assert(!_mem_cursor.is_valid());

    _mem_cursor.prev();
    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      uint64_t foffset = found->first;
      if (foffset + found->second == offset) {
        // a contiguous block: paste them together
        offset = foffset;
        size += found->second;
        _remove_chunk(foffset, found->second);
      } else
        _mem_cursor.next();
    } else
      _mem_cursor.first();

    if (_mem_cursor.is_valid()) {
      BigSizeKey* found = (BigSizeKey*)_mem_cursor.key().data();
      uint64_t foffset = found->first;
      if (foffset == offset + size) {
        // a contiguous block: paste them together
        size += found->second;
        _remove_chunk(foffset, found->second);
      }
    }

    _add_chunk(offset, size);
  }
};

}  // namespace leaves

#endif  // _LEAVES_IBIGVALUE_HPP