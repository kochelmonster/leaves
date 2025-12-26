#ifndef _LEAVES_BIGMEMORY_HPP
#define _LEAVES_BIGMEMORY_HPP

#include <boost/endian/arithmetic.hpp>
#include "_hash.hpp"
#include "_util.hpp"

namespace leaves {

template <typename TCursor>
struct _BigMemory {
  using Traits = typename TCursor::Traits;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using uint64_e = typename Traits::uint64_e;
  using tid_t = typename Traits::tid_t;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto MAX_BLOCK_SIZE = Traits::BLOCK_SIZES[Traits::BLOCK_SIZES_COUNT - 1];
  static constexpr auto BIG_VALUE_FLAG = uint16_t(1) << 15;

  struct SizeKey {
    boost::endian::big_uint64_t size;
    uint64_t offset;
  };

  struct OffsetKey {
    boost::endian::big_uint64_t offset;
    uint64_t size;
  };

  struct ValueBlock {
    tid_t txn_id;
  };

  struct BigValue {
    AreaSlice area;
    uint64_e value_size;
    template<typename DB_>
    char* data(DB_* db) {
      auto ptr = db->resolve(offset_t(area.offset()));
      return (char*)ptr;
    }
  };

  struct CursorTraits : public Traits {
    typedef ::NullHasher Hasher;
  };

  DB* _db;
  TCursor _size_cursor;
  TCursor _offset_cursor;
  _BigMemory(DB* db, offset_e* size_root, offset_e* offset_root)
      : _db(db), _size_cursor(db, size_root), _offset_cursor(db, offset_root) {}

  template <typename LeafNode>
  static uint16_t modify_size(uint16_t key, uint64_t size) {
    key &= 0xff;
    if (sizeof(LeafNode) + size + key > LeafNode::MAX_SIZE)
      return sizeof(LeafNode) + sizeof(BigValue) + key;
    return size;
  }

  void _add_chunk(offset_t offset, size_t size) {
    SizeKey skey{size, offset._offset};
    OffsetKey okey{offset._offset, size};
    _size_cursor.find(Slice(&skey, sizeof(skey)));
    _offset_cursor.find(Slice(&okey, sizeof(okey)));

    assert(!_size_cursor.is_valid());
    assert(!_offset_cursor.is_valid());

    ValueBlock vblock;
    _db->mark_for_recycle(vblock);
    Slice vblock_slice(&vblock, sizeof(vblock));

    _size_cursor.value(vblock_slice);
    _offset_cursor.value(vblock_slice);
  }

  void _remove_chunk(offset_t offset, size_t size) {
    SizeKey skey{size, offset._offset};
    OffsetKey okey{offset._offset, size};
    _size_cursor.find(Slice(&skey, sizeof(skey)));
    _offset_cursor.find(Slice(&okey, sizeof(okey)));

    assert(_size_cursor.is_valid());
    assert(_offset_cursor.is_valid());

    _size_cursor.remove();
    _offset_cursor.remove();
  }

  AreaSlice alloc(uint64_t size) {
    size = padding(size, MAX_BLOCK_SIZE);
    uint32_t found_size;
    offset_t found_offset;

    // find from big memory storage
    SizeKey skey;
    skey.size = size;
    skey.offset = 0;
    _size_cursor.find(Slice(&skey, sizeof(skey)));

    SizeKey* found = nullptr;
    for (int i = 0; i < 10 && _size_cursor.is_valid(); i++) {
      ValueBlock* vblock = (ValueBlock*)_size_cursor.value().data();
      if (_db->may_recycle(*vblock)) {
        found = (SizeKey*)_size_cursor.key().data();
        assert(found->size >= size);
        break;
      }
      _size_cursor.next();
    }

    if (found) {
      OffsetKey okey;
      okey.offset = found_offset = found->offset;
      okey.size = found_size = found->size;
      _offset_cursor.find(Slice(&okey, sizeof(okey)));
      assert(_offset_cursor.is_valid());

      _offset_cursor.remove();
      _size_cursor.remove();
    } else {
      // allocate new multi-area
      uint64_t psize = padding(size, AREA_SIZE);
      auto area = _db->alloc_multi_area(psize);
      found_offset = area->content_offset();
      found_size = area->end() - found_offset;
    }

    uint32_t delta = found_size - size;
    if (delta >= MAX_BLOCK_SIZE) {
      // enough space left -> reuse the rest
      _add_chunk(found_offset + size, delta);
      found_size -= delta;
    }

    return AreaSlice{found_offset, found_size};
  }

  void free(const AreaSlice& slice) {
    _add_chunk(slice.offset(), slice.size());
  }

  void defrag() {
    _offset_cursor.first();
    offset_e last = 0;
    size_t size = 0;
    size_t merged = 0;
    while (_offset_cursor.is_valid()) {
      OffsetKey* okey = (OffsetKey*)_offset_cursor.key().data();
      if (okey->offset == last + size) {
        size += okey->size;
        SizeKey skey{okey->size, okey->offset};
        _size_cursor.find(Slice(&skey, sizeof(skey)));
        assert(_size_cursor.is_valid());
        _size_cursor.remove();
        _offset_cursor.remove();
        merged++;
      }
      else if (merged) {
        _offset_cursor.prev();
        OffsetKey* okey = (OffsetKey*)_offset_cursor.key().data();
        assert(okey->offset == last);
        SizeKey skey{okey->size, okey->offset};
        _size_cursor.find(Slice(&skey, sizeof(skey)));
        assert(_size_cursor.is_valid());
        _size_cursor.remove();
        _offset_cursor.remove();
        _add_chunk(last, size);
        merged = 0;
      }
      _offset_cursor.next();
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_BIGMEMORY_HPP