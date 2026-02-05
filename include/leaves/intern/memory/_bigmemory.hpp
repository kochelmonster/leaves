#ifndef _LEAVES_BIGMEMORY_HPP
#define _LEAVES_BIGMEMORY_HPP

#include <boost/endian/arithmetic.hpp>

#include "../db/_check.hpp"
#include "../core/_hash.hpp"
#include "../core/_util.hpp"

namespace leaves {

template <typename TCursor>
struct _BigMemory {
  // Dummy structure for chunk pointers
  struct Chunk {};

  using Traits = typename TCursor::Traits;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using uint64_e = typename Traits::uint64_e;
  using chunk_ptr = typename Traits::template Pointer<Chunk>;

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto MAX_PAGE_SIZE =
      Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
  static constexpr auto BIG_VALUE_FLAG = uint16_t(1) << 15;

  struct FreeKey {
    boost::endian::big_uint64_t size;
    uint64_t offset;
  };

  struct ValueBlock {
    tid_t txn_id;
  };

  struct BigValue {
    using uint32_e = typename Traits::uint32_e;
    using uint64_e = typename Traits::uint64_e;
    uint64_e chunk_offset;
    uint32_e value_size;
    template <typename DB_>
    chunk_ptr data(DB_* db) {
      offset_t temp_offset(chunk_offset);
      return db->template resolve<Chunk>(&temp_offset, READ);
    }
  };

  struct CursorTraits : public Traits {
  };

  DB* _db;
  TCursor _free_cursor;

  _BigMemory(DB* db, offset_e* free_bigmem_root)
      : _db(db), _free_cursor(db, free_bigmem_root) {}

  template <typename LeafNode>
  static uint16_t modify_size(uint16_t key, uint64_t size) {
    key &= 0xff;
    if (sizeof(LeafNode) + size + key > MAX_PAGE_SIZE)
      return sizeof(BigValue);
    return size;
  }

  void _add_chunk(offset_t offset, size_t size, bool has_successor, bool freed) {
    uint64_t offset_val = offset._offset & ~uint64_t(1);
    if (has_successor) {
      offset_val |= 1;
    }
    
    FreeKey fkey{size, offset_val};
    _free_cursor.find(Slice(&fkey, sizeof(fkey)));
    assert(!_free_cursor.is_valid());

    ValueBlock vblock{};
    if (freed) _db->mark_for_recycle(vblock);
    Slice vblock_slice(&vblock, sizeof(vblock));
    _free_cursor.value(vblock_slice);
  }

  void reset(offset_e* free_bigmem_root) {
    _free_cursor.set_root(free_bigmem_root);
  }

  void alloc(uint64_t size, BigValue* result) {
    uint64_t total_size = sizeof(FreeKey) + size;
    uint64_t padded_size = padding(total_size, MAX_PAGE_SIZE);
    uint64_t found_size;
    offset_t found_offset;
    bool has_successor = false;

    FreeKey fkey;
    fkey.size = padded_size;
    fkey.offset = 0;
    _free_cursor.find(Slice(&fkey, sizeof(fkey)));
    _free_cursor.next();

    FreeKey* found = nullptr;
    for (int i = 0; i < 10 && _free_cursor.is_valid(); i++) {
      ValueBlock* vblock = (ValueBlock*)_free_cursor.value().data();
      if (_db->may_recycle(*vblock)) {
        found = (FreeKey*)_free_cursor.key().data();
        assert(found->size >= padded_size);
        break;
      }
      _free_cursor.next();
    }

    if (found) {
      uint64_t offset_with_flag = found->offset;
      has_successor = (offset_with_flag & 1) != 0;
      found_offset = offset_with_flag & ~uint64_t(1);
      found_size = found->size;
      _free_cursor.remove();
    } else {
      uint64_t psize = padding(padded_size, AREA_SIZE);
      auto area = _db->alloc_multi_area(psize);
      found_offset = area->content_offset();
      found_size = area->end() - found_offset;
      has_successor = false;
    }
    _db->prefetch(&found_offset, WRITE);

    uint64_t delta = found_size - padded_size;
    if (delta >= MAX_PAGE_SIZE) {
      _add_chunk(found_offset + padded_size, delta, has_successor, false);
      found_size = padded_size;
      has_successor = true;
    }

    auto header_ptr = (FreeKey*)(char*)_db->template resolve<Chunk>(&found_offset, WRITE);
    header_ptr->size = found_size;
    header_ptr->offset = found_offset | (has_successor ? 1 : 0);

    result->chunk_offset = found_offset + sizeof(FreeKey);
    result->value_size = size;
  }

  void free(const BigValue* bvalue) {
    offset_t header_offset = offset_t(bvalue->chunk_offset - sizeof(FreeKey));
    auto header_block = _db->template resolve<Chunk>(&header_offset, READ);
    auto header_ptr = (FreeKey*)(char*)header_block;
    uint64_t offset_with_flag = header_ptr->offset;
    uint64_t chunk_offset = offset_with_flag & ~uint64_t(1);
    uint64_t chunk_size = header_ptr->size;
    bool has_successor = (offset_with_flag & 1) != 0;
    _add_chunk(offset_t(chunk_offset), chunk_size, has_successor, true);
  }

  template <typename txn_ptr>
  void defrag(txn_ptr txn) {
    TCursor iter_cursor(_db, &txn->free_bigmem_root);
    TCursor lookup_cursor(_db, &txn->free_bigmem_root);
    
    iter_cursor.first();
    
    while (iter_cursor.is_valid()) {
      ValueBlock* vblock = (ValueBlock*)iter_cursor.value().data();
      if (!_db->may_recycle(*vblock)) {
        iter_cursor.next();
        continue;
      }

      ValueBlock current_vblock = *vblock;

      FreeKey current_key = *(FreeKey*)iter_cursor.key().data();
      uint64_t offset_with_flag = current_key.offset;
      uint64_t current_offset = offset_with_flag & ~uint64_t(1);
      uint64_t current_size = current_key.size;
      bool has_successor = (offset_with_flag & 1) != 0;

      uint64_t total_size = current_size;
      bool found_mergeable = false;
      
      while (has_successor) {
        uint64_t next_offset = current_offset + total_size;
        offset_t next_offset_t(next_offset);
        auto next_header = _db->template resolve<FreeKey>(&next_offset_t, READ);
        FreeKey next_key = *next_header;
        
        lookup_cursor.find(Slice((char*)next_header, sizeof(FreeKey)));
        if (!lookup_cursor.is_valid()) {
          break;
        }
        
        ValueBlock* next_vblock = (ValueBlock*)lookup_cursor.value().data();
        if (!_db->may_recycle(*next_vblock)) {
          break;
        }
        
        lookup_cursor.remove();
        total_size += next_header->size;
        has_successor = (next_header->offset & 1) != 0;
        found_mergeable = true;
      }
      
      if (found_mergeable) {
        // Update the header of the merged chunk
        offset_t current_offset_t(current_offset);
        auto merged_header = _db->template resolve<FreeKey>(&current_offset_t, WRITE);
        merged_header->size = total_size;
        uint64_t offset_val = current_offset & ~uint64_t(1);
        if (has_successor) {
          offset_val |= 1;
        }
        merged_header->offset = offset_val;
        
        // Remove old entry and add merged entry using the lookup cursor.
        // (Avoids relying on iter_cursor's internal stack after mutations.)
        lookup_cursor.find(Slice(&current_key, sizeof(current_key)));
        assert(lookup_cursor.is_valid());
        lookup_cursor.remove();

        FreeKey merged_key{total_size, offset_val};
        lookup_cursor.find(Slice(&merged_key, sizeof(merged_key)));
        assert(!lookup_cursor.is_valid());
        Slice vblock_slice(&current_vblock, sizeof(current_vblock));
        lookup_cursor.value(vblock_slice);

        iter_cursor.first();
      } else {
        iter_cursor.next();
      }
    }
  }
};

}  // namespace leaves

#endif  // _LEAVES_BIGMEMORY_HPP