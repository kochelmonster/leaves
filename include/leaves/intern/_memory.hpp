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

namespace leaves {

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

constexpr size_t padding(size_t a, size_t b) {
  return ((a + b - 1) / b) * b;  // Round up to the next multiple of b
}

template <typename BlockHeader>
struct _DumpSlot {
  using ptr_t = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  using bsize_t = typename BlockHeader::bsize_t;

  template <typename Storage>
  ptr_t pop(Storage& storage) {
    assert(start);
    assert(free > 0);
    free--;
    ptr_t result = storage.template resolve<BlockHeader>(start);
    start = start == end ? (end = 0) : result->next_free;
    return result;
  }

  template <typename Storage>
  void push(ptr_t block, Storage& storage) {
    auto baddr = storage.template resolve<BlockHeader>(block);
    block->next_free = start;
    start = baddr;

    /* end is never 0 because pushing the first block
       will be done by an insert operation that constructs the Slot 
       with a valid end */
    assert(end != 0); 
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

    auto block = storage.template resolve<BlockHeader>(end);
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
  using bsize_t = typename BlockHeader::bsize_t;
  using ptr_t = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  const static size_t END_IDX = Slot1024Trait::END_IDX;
  const static size_t AREA_SIZE = 256 * K;

  struct FreeBlock {
    // offset to the end current area
    offset_t end_current_area;

    // the offset to the next free memory inside current_area
    // current_area <= next_free < area_size!
    offset_t next_free;

    template <typename T>
    void clone(const T& src) {
      end_current_area = src.end_current_area;
      next_free = src.next_free;
    }

    void reset() { end_current_area = next_free = 0; }
  };

  typedef _DumpSlot<BlockHeader> DumpSlot;
  typedef _SparseArray<DumpSlot, padding(END_IDX, 64)> DumpArray;
  const static size_t FULL_SIZE =
      2 * sizeof(FreeBlock) + DumpArray::space(END_IDX);

  FreeBlock small, big;
  DumpArray dumps;

  bsize_t space() const { return 2 * sizeof(FreeBlock) + dumps.space(); }

  template <typename Storage>
  ptr_t alloc(bsize_t space, Storage& storage) {
    int dix = calc_dump_index(space);
    if (dix < 0) return nullptr;

    ptr_t result;
    if (dumps.get(dix)) {
      DumpSlot& dump = dumps[dix];
      result = dump.pop(storage);
      assert(result);
      if (dump.free == 0) dumps.remove(dix);
      return result;
    }

    bsize_t bsize = BLOCK_SIZES[dix];
    dix = dumps.bits.next(dix);
    if (dix >= 0) {
      DumpSlot& dump = dumps[dix];
      result = dump.pop(storage);
      assert(result);
      if (dump.free == 0) dumps.remove(dix);
      // split the block and take only as much space as needed
      ptr_t waste = (ptr_t)(((char*)result) + bsize);
      waste->block_size = result->block_size - bsize;
      free(waste, storage);
      result->block_size = bsize;
      return result;
    }

    // alloc a new one
    FreeBlock& fblk = bsize <= 4*K ? small : big;
    result = storage.template resolve<BlockHeader>(fblk.next_free);
    if (fblk.next_free + bsize <= fblk.end_current_area) {
      fblk.next_free += bsize;
      result->block_size = bsize;
      return result;
    }

    assert(fblk.next_free + bsize > fblk.end_current_area);
    // put the rest in the dump and create a new area
    if (fblk.next_free < fblk.end_current_area) {
      result->block_size = fblk.end_current_area - fblk.next_free;
      free(result, storage);
    }

    offset_t narea = storage.alloc_area(AREA_SIZE);
    result = storage.template resolve<BlockHeader>(narea);
    result->block_size = bsize;
    fblk.next_free = narea + bsize;
    fblk.end_current_area = narea + AREA_SIZE;
    return result;
  }

  template <typename Storage>
  bool free(ptr_t block, Storage& storage) {
    auto dix = calc_dump_index(block->block_size);
    if (dix < 0) return false;
    if (!dumps.get(dix)) {
      auto addr = storage.template resolve<BlockHeader>(block);
      dumps.insert(dix, DumpSlot{.start = addr, .end = addr, .free = 1});
    } else {
      dumps[dix].push(block, storage);
    }
    return true;
  }

  template <typename T>
  void pclone(const T& src) {
    small.clone(src.small);
    big.clone(src.big);
  }

  template <typename T>
  void unify(const T& src) {
    dumps.bits.unify(src.dumps.bits);
  }

  template <typename T>
  void add(const T& src) {
    for (auto it = src.dumps.begin(); it != src.dumps.end(); ++it) {
      assert(dumps.get(it.index));
      dumps[it.index] = *it;
    }
  }

  void reset() {
    small.reset();
    big.reset();
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
