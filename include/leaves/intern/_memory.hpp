/*
Transaction save memory managment.

If a transactions is rolled back or the machine crashes,
the memory must return to the previous saved state, without
any blocks lost.

All important operations are done in DumpSlot

Constraint A:
It is important that the original members of the linked list,
may not change their "next" pointer within the transaction.
Keep in mind, Blocks that are added to the list come from
the trie. Their "next" pointer can be savely changed: In case
of a rollback, they still stay in the trie and the "next"
pointer is not used.

Some Operations and how it is ensured that "state recovery" is accomplished:

merge(append a list of free blocks to another one)
    This operation breaks Constraint A: While appending the "end block"
    will point to the "start block" of the appending list.
    The solution is: The end of the linked list is not defined by the
    "next" pointer of the "end block" is NULL but by the "end" pointer
    of the DumpSlot. That means if the "end" pointer of DumpSlots points
    to a block this block is the end of the list.
*/

#ifndef _LEAVES__MEMORY_HPP
#define _LEAVES__MEMORY_HPP

#include <array>
#include <cstddef>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

template <typename BlockHeader>
struct _DumpSlot {
  using block_ptr = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  using bsize_t = typename BlockHeader::bsize_t;

  template <typename Storage>
  block_ptr pop(Storage& storage) {
    assert(start);
    assert(free > 0);
    free--;
    block_ptr result = storage.template resolve(start);
    start = start == end ? (end = 0) : result->next_free;
    return result;
  }

  template <typename Storage>
  void push(block_ptr block, Storage& storage) {
    auto baddr = storage.template resolve(block);
    block->next_free = start;
    start = baddr;

    /* end is never 0 because pushing the first block
       will be done by an insert operation that constructs the Slot
       with a valid end */
    assert((uint64_t)end != 0);
    free++;
  }

  template <typename T, typename Storage>
  void merge(const T& src, Storage& storage) {
    if (!start) {
      assert(!end);
      start = src.start;
      end = src.end;
      free = src.free;
      return;
    }

    auto block = storage.template resolve(end);
    block->next = src.start;
    end = src.end;
    free += src.free;
  }

  offset_t start;
  offset_t end;
  bsize_t free;
};

struct Slot128Trait {
  const static size_t MULTIPLIER = 128;
  const static size_t START = 0;
  const static size_t END = 4 * K;
  const static size_t START_IDX = 0;
  const static size_t END_IDX = ((END - START) / MULTIPLIER);
};

struct Slot512Trait {
  const static size_t MULTIPLIER = 512;
  const static size_t START = Slot128Trait::END;
  const static size_t END = 16 * K;
  const static size_t START_IDX = Slot128Trait::END_IDX;
  const static size_t END_IDX = START_IDX + ((END - START) / MULTIPLIER);
};

struct Slot1024Trait {
  const static size_t MULTIPLIER = 1024;
  const static size_t START = Slot512Trait::END;
  const static size_t END = 64 * MULTIPLIER;
  const static size_t START_IDX = Slot512Trait::END_IDX;
  const static size_t END_IDX = START_IDX + ((END - START) / MULTIPLIER);
};

typedef ::std::array<size_t, Slot1024Trait::END_IDX> blocksizes_t;

template <typename Trait>
constexpr bool set_vector(int idx, blocksizes_t& result) {
  if (Trait::START_IDX <= idx && idx < Trait::END_IDX) {
    result[idx] =
        Trait::START + (1 + idx - Trait::START_IDX) * Trait::MULTIPLIER;
    return true;
  }
  return false;
}

constexpr blocksizes_t generate_block_sizes() {
  blocksizes_t result{0};
  for (int i = 0; i < result.size(); i++) {
    if (set_vector<Slot128Trait>(i, result)) continue;
    if (set_vector<Slot512Trait>(i, result)) continue;
    set_vector<Slot1024Trait>(i, result);
  }
  return result;
}

template <typename Trait>
constexpr bool fit(size_t size) {
  return Trait::START < size && size <= Trait::END;
}

template <typename Trait>
constexpr int calc_dump_index(size_t size) {
  assert(size > 0);
  return Trait::START_IDX + (size - 1 - Trait::START) / Trait::MULTIPLIER;
}

constexpr int calc_dump_index(size_t size) {
  if (fit<Slot128Trait>(size)) return calc_dump_index<Slot128Trait>(size);
  if (fit<Slot512Trait>(size)) return calc_dump_index<Slot512Trait>(size);
  if (fit<Slot1024Trait>(size)) return calc_dump_index<Slot1024Trait>(size);
  return -1;
}

const blocksizes_t BLOCK_SIZES = generate_block_sizes();

template <typename BlockHeader>
struct _MemManager : public BlockHeader {
  typedef BlockHeader Base;
  using bsize_t = typename BlockHeader::bsize_t;
  using block_ptr = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  typedef _MemManager<BlockHeader> MemManager;
  typedef typename BlockHeader::template Pointer<MemManager> ptr;

  const static size_t END_IDX = Slot1024Trait::END_IDX;
  const static size_t AREA_SIZE = 256 * K;

  struct FreeBlock {
    // offset to the end current area
    offset_t end_current_area;

    // the offset to the next free memory inside current_area
    // current_area <= next_free < area_size!
    offset_t next_free;

    void copy(const FreeBlock& src) {
      end_current_area = src.end_current_area;
      next_free = src.next_free;
    }

    void reset() { end_current_area = next_free = 0; }
  };

  typedef _DumpSlot<BlockHeader> DumpSlot;
  typedef _SparseArray<DumpSlot, padding(END_IDX, 64)> DumpArray;
  const static size_t FULL_SIZE =
      2 * sizeof(FreeBlock) + DumpArray::space(END_IDX);

  FreeBlock b128, b512, b1024;
  DumpArray dumps;

  // the space needed for MemManager
  bsize_t space() const { return 2 * sizeof(FreeBlock) + dumps.space(); }

  template <typename Storage>
  block_ptr alloc(bsize_t space, Storage& storage) {
    int dix = calc_dump_index(space);
    if (dix < 0) return nullptr;

    bsize_t bsize = BLOCK_SIZES[dix];
    block_ptr result;
    if (dumps.get(dix)) {
      DumpSlot& dump = dumps[dix];
      result = dump.pop(storage);
      assert(result);
      if (dump.free == 0) dumps.remove(dix);
      // Block size is maybe wrong (see the next if + rollback)
      // but the dump is right
      result->block_size = bsize;
      return result;
    }

    dix = dumps.bits.next(dix);
    if (dix > 0) {
      DumpSlot& dump = dumps[dix];
      result = dump.pop(storage);
      assert(result);
      if (dump.free == 0) dumps.remove(dix);

      // bsize must be a multiple of the current multiplier
      bsize = padding(bsize, BLOCK_SIZES[dix] - BLOCK_SIZES[dix - 1]);

      // split the block and take only as much space as needed
      bsize_t waste_block_size = BLOCK_SIZES[dix] - bsize;
      if (waste_block_size) {
        block_ptr waste = (block_ptr)(((char*)result) + bsize);
        waste->block_size = waste_block_size;
        free(waste, storage);
      }
      result->block_size = bsize;
      return result;
    }

    // alloc a new one
    FreeBlock& fblk = bsize <= Slot128Trait::END ? b128 : (
      bsize <= Slot512Trait::END ? b512 : b1024);

    result = storage.template resolve(fblk.next_free);
    if ((uint64_t)fblk.next_free + bsize <= (uint64_t)fblk.end_current_area) {
      fblk.next_free += bsize;
      result->block_size = bsize;
      return result;
    }

    assert((uint64_t)fblk.next_free + bsize > (uint64_t)fblk.end_current_area);
    // put the rest in the dump and create a new area
    if ((uint64_t)fblk.next_free < (uint64_t)fblk.end_current_area) {
      result->block_size =
          (uint64_t)fblk.end_current_area - (uint64_t)fblk.next_free;
      free(result, storage);
    }

    offset_t narea = storage.alloc_area(AREA_SIZE);
    result = storage.template resolve(narea);
    result->block_size = bsize;
    fblk.next_free = (uint64_t)narea + bsize;
    fblk.end_current_area = (uint64_t)narea + AREA_SIZE;
    return result;
  }

  template <typename Storage>
  bool free(block_ptr block, Storage& storage) {
    auto dix = calc_dump_index(block->block_size);
    if (dix < 0) return false;
    assert(block->block_size == BLOCK_SIZES[dix]);
    if (!dumps.get(dix)) {
      auto addr = storage.template resolve(block);
      dumps.insert(dix, DumpSlot{.start = addr, .end = addr, .free = 1});
    } else {
      dumps[dix].push(block, storage);
    }
    return true;
  }

  template <typename Storage>
  ptr clone(Storage& storage) const {
    ptr mem =
        storage.alloc(sizeof(MemManager) + dumps.space() - sizeof(dumps.bits));
    copy(*mem, *this, dumps.space() - sizeof(dumps.bits));
    return mem;
  }

  void pcopy(const ptr& src) {
    b128.copy(src->b128);
    b512.copy(src->b512);
    b1024.copy(src->b1024);
  }

  void unify(const ptr& src) { dumps.bits.unify(src->dumps.bits); }

  void add(const ptr& src) {
    for (auto it = src->dumps.begin(); it != src->dumps.end(); ++it) {
      assert(dumps.get(it.index));
      dumps[it.index] = *it;
    }
  }

  void reset() {
    b128.reset();
    b512.reset();
    b1024.reset();
    dumps.init();
  }

  template <typename T, typename Storage>
  void merge(const T& src, Storage& storage) {
    int i = 0;
    for (auto it = src.begin(); it != src.end(); ++it) {
      dumps[i].merge(src.dumps[i], storage);
    }
  }
};

struct MemStatistics {
  const static size_t END_IDX = Slot1024Trait::END_IDX;

  struct Slot {
    size_t block_size;
    size_t count;
    size_t free;
  };

  typedef _SparseArray<Slot, padding(END_IDX, 64)> SlotArray;  
  union {
    SlotArray slots;
    char buffer[SlotArray::space(END_IDX)];
  };

  MemStatistics() { slots.init(); }

  void add(size_t block_size, size_t count, size_t free=0) {
    int dix = calc_dump_index(block_size);
    if (dix < 0) return;

    if (!slots.get(dix)) {
      slots.insert(dix, Slot{.block_size = block_size, .count = count, .free = free});
    } else {
      Slot& slot = slots[dix];
      assert(slot.block_size == block_size);
      slot.count += count;
      slot.free += free;
    }
  }
};

}  // namespace leaves

/*
transaction  memory     replication    endian
  x           mmap         x             x     replicable database
  x           mmap         -             -     non replicable database
  x           indexdb      x             x     replicable database browser
  x           indexdb      -             -     non replicable database browser


  -           heapblock   -         -           x     persistant and
transferable storage
  -           heao        x         -           -     in memory storage
*/

#endif  // _LEAVES__MEMORY_HPP
