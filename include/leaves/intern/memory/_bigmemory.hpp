#ifndef _LEAVES_BIGMEMORY_HPP
#define _LEAVES_BIGMEMORY_HPP

#include "../core/_port.hpp"
#include "../db/_aspect.hpp"
#include "../db/_check.hpp"
#include "_memory.hpp"

namespace leaves {

struct _FreeKey {
  _big_uint64_t size;
  uint64_t offset;
};

struct _BigValueChunk {};

struct _BigValue {
  _little_uint64_t chunk_offset;  // offset into chunk storage
  _little_uint32_t value_size;    // size of the actual value data

  template <typename DB_>
  auto data(DB_* db) {
    offset_t temp_offset(chunk_offset);
    return db->template resolve<_BigValueChunk>(&temp_offset, READ);
  }
};

template <typename TCursor>
struct _BigMemory {
  using Chunk = _BigValueChunk;

  using Traits = typename TCursor::Traits;
  using DB = typename Traits::DB;
  using offset_e = typename Traits::offset_e;
  using uint64_e = typename Traits::uint64_e;
  using chunk_ptr = typename Traits::template Pointer<Chunk>;

  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto MAX_PAGE_SIZE =
      Traits::PAGE_SIZES[Traits::PAGE_SIZES_COUNT - 1];
  static constexpr auto BIG_VALUE_FLAG = uint16_t(1) << 15;

  using FreeKey = _FreeKey;
  using BigValue = _BigValue;

  struct ValueBlock {
    tid_t txn_id;
  };

  struct CursorTraits : public Traits {
  };

  DB* _db;
  TCursor _free_cursor;

  _BigMemory(DB* db, offset_e* free_bigmem_root)
      : _db(db), _free_cursor(db, free_bigmem_root) {}

  void set_ctx(typename DB::TxnContext* ctx, tid_t active_tid) {
    _free_cursor._ctx = ctx;
    _free_cursor._active_tid = active_tid;
  }

  template <typename LeafNode>
  static uint16_t modify_size(uint16_t key, uint64_t size,
                              size_t big_inline_size = sizeof(BigValue)) {
    key &= 0xff;
    if (sizeof(LeafNode) + size + key > MAX_PAGE_SIZE)
      return big_inline_size;
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
    if (freed) vblock.txn_id = _free_cursor._active_tid;
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
      if (vblock->txn_id < _free_cursor._ctx->_recycle_txn_id) {
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
      uint64_t psize = padding(padded_size + sizeof(Area), AREA_SIZE);
      auto area = _db->_alloc_multi_area(psize, _free_cursor._ctx);
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

  struct _defrag_root {
    offset_e* root;
    typename DB::TxnContext* ctx;
    tid_t active_tid;
  };

  // Full defrag lifecycle: create cursors, start transactions on all
  // contexts, merge adjacent free chunks, then commit everything.
  void defrag() {
    using cursor_ptr = typename DB::cursor_ptr;

    cursor_ptr cursors[256];
    for (uint8_t i = 0; i < _db->_num_contexts; i++) {
      cursors[i] = _db->create_cursor();
      cursors[i]->start_transaction(false, TransactionOrigin::defrag);
    }

    _defrag_root roots_buf[256];
    size_t num_roots = 0;
    for (uint8_t i = 0; i < _db->_num_contexts; i++) {
      auto& c = cursors[i];
      if (c->_txn->free_bigmem_root) {
        roots_buf[num_roots++] = {
            &c->_txn->free_bigmem_root, c->_ctx, c->_active_tid};
      }
    }

    if (num_roots == 0) {
      for (uint8_t i = 0; i < _db->_num_contexts; i++)
        cursors[i]->rollback(TransactionOrigin::defrag);
      return;
    }

    reset(roots_buf[0].root);
    set_ctx(roots_buf[0].ctx, roots_buf[0].active_tid);
    defrag(roots_buf, num_roots);
    _db->flush();

    for (uint8_t i = 0; i < _db->_num_contexts; i++)
      cursors[i]->commit(false, TransactionOrigin::defrag);
  }

  void defrag(const _defrag_root* roots, size_t num_roots) {
    for (size_t ri = 0; ri < num_roots; ri++) {
      auto& iter_root = roots[ri];
      TCursor iter_cursor(_db, iter_root.root);
      iter_cursor._ctx = iter_root.ctx;
      iter_cursor._active_tid = iter_root.active_tid;

      iter_cursor.first();

      while (iter_cursor.is_valid()) {
        ValueBlock* vblock = (ValueBlock*)iter_cursor.value().data();
        if (!(vblock->txn_id < iter_root.ctx->_recycle_txn_id)) {
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

          bool found = false;
          // Search all roots for the successor chunk
          for (size_t si = 0; si < num_roots; si++) {
            auto& search_root = roots[si];
            TCursor lookup_cursor(_db, search_root.root);
            lookup_cursor._ctx = search_root.ctx;
            lookup_cursor._active_tid = search_root.active_tid;
            lookup_cursor.find(Slice((char*)next_header, sizeof(FreeKey)));
            if (lookup_cursor.is_valid()) {
              ValueBlock* next_vblock = (ValueBlock*)lookup_cursor.value().data();
              if (next_vblock->txn_id < search_root.ctx->_recycle_txn_id) {
                lookup_cursor.remove();
                total_size += next_header->size;
                has_successor = (next_header->offset & 1) != 0;
                found = true;
                found_mergeable = true;
                break;
              }
            }
          }
          if (!found) break;
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

          // Remove old entry and add merged entry using a fresh lookup cursor.
          // (Avoids relying on iter_cursor's internal stack after mutations.)
          TCursor lookup_cursor(_db, iter_root.root);
          lookup_cursor._ctx = iter_root.ctx;
          lookup_cursor._active_tid = iter_root.active_tid;
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
  }
};

}  // namespace leaves

#endif  // _LEAVES_BIGMEMORY_HPP