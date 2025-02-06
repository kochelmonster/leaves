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
#include <cassert>
#include <cstddef>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

// One page inside the garbage queue
template <typename BlockHeader>
struct _GarbageContainer : public BlockHeader {
  static const size_t SIZE = 4096;
  typedef BlockHeader Base;
  typedef typename BlockHeader::template Pointer<_GarbageContainer> ptr;
  using offset_t = typename BlockHeader::offset_t;
  using tid_t = typename BlockHeader::tid_t;

  struct FreeBlock {
    offset_t link;  // link to the page
    tid_t tid;      // the transaction that freed the page
  };

  static const size_t BLOCK_COUNT =
      (SIZE - sizeof(offset_t) - sizeof(Base)) / sizeof(FreeBlock);

  offset_t next;
  FreeBlock blocks[BLOCK_COUNT];
};

/*
Saves Freed pages of a given Blocksize.
The pages are saved in a queue,
Newly freed page are added to the end of the queue
recycled pages are consumed from the start of the queue
Each queue is linked to a GarbageSlot.
Each Transaction has its own GarbageSlot pointing to
different ends and starts of the queue.
*/

template <typename BlockHeader>
struct _GarbageSlot {
  using block_ptr = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  using bsize_t = typename BlockHeader::bsize_t;
  typedef _GarbageSlot<BlockHeader> GarbageSlot;
  typedef _GarbageContainer<BlockHeader> GarbageContainer;
  using garb_ptr = typename GarbageContainer::ptr;

  template <typename Storage>
  block_ptr pop(Storage& storage) {
    if (count == 0) return nullptr;

    assert(!(ostart == oend && istart == iend));

    garb_ptr front = storage.resolve(ostart);
    if (!storage.template may_recycle(front->blocks[istart])) return nullptr;

    assert(front->blocks[istart].link != 0);
    block_ptr result = storage.resolve(front->blocks[istart].link);
    count--;
    istart++;

    if (ostart == oend && istart >= iend) {
      assert(count == 0);
      if (result->block_size != GarbageContainer::SIZE) {
        istart = iend = 0;
        ostart = oend = 0;
        storage.free(front);
      } else {
        istart = iend = 0;
      }
    } else if (istart >= GarbageContainer::BLOCK_COUNT) {
      ostart = front->next;
      istart = 0;
      storage.free(front);
    }
    return result;
  }

  template <typename Storage>
  void push(const block_ptr& block, Storage& storage) {
    garb_ptr back;

    assert(oend != 1);  // locked

    if (oend) {
      back = storage.resolve(oend);
      if (iend >= GarbageContainer::BLOCK_COUNT) {
        garb_ptr new_back = storage.alloc(GarbageContainer::SIZE);
        oend = back->next = storage.resolve(new_back);
        iend = 0;
        back = new_back;
      }
    } else {
      assert(ostart == 0);
      assert(istart == 0);
      assert(iend == 0);
      assert(count == 0);
      back = storage.alloc(GarbageContainer::SIZE);
      oend = ostart = storage.resolve(back);
    }
    back->blocks[iend].link = storage.resolve(block);
    assert(back->blocks[iend].link != 0);
    storage.template mark_for_recycle(back->blocks[iend]);
    iend++;
    count++;
  }

  template <typename Caller, typename Storage>
  void iter(Storage& storage, Caller call) {
    offset_t ocontainer = ostart;
    uint16_t index = istart;

    while (true) {
      garb_ptr container = storage.resolve(ocontainer);

      if (ocontainer == oend) {
        for (; index < iend; index++) {
          call(container->blocks[index]);
        }
        break;
      }

      for (; index < GarbageContainer::BLOCK_COUNT; index++)
        call(container->blocks[index]);

      index = 0;
      ocontainer = container->next;
    }
  }

  offset_t ostart;  // offset of the start
  offset_t oend;    // offset of the end
  uint16_t istart;  // index inside the start
  uint16_t iend;    // index inside the end
  bsize_t count;
};

const static size_t BLOCK_SIZES[] = {
    32,    64,    96,    128,   160,   192,   224,   256,   288,   320,   352,
    384,   416,   448,   480,   512,   576,   640,   704,   768,   832,   896,
    960,   1024,  1152,  1280,  1408,  1536,  1664,  1792,  1920,  2048,  2304,
    2560,  2816,  3072,  3328,  3584,  3840,  4096,  4608,  5120,  5632,  6144,
    6656,  7168,  7680,  8192,  9216,  10240, 11264, 12288, 13312, 14336, 15360,
    16384, 18432, 20480, 22528, 24576, 26624, 28672, 30720, 32768, 36864, 40960,
    45056, 49152, 53248, 57344, 61440, 65536};

static const size_t BLOCK_COUNT = sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

inline constexpr int assign_block(size_t size) {
  assert(size > 0);
  size--;
  for (int i = 9; i < 17; ++i) {
    if ((size >> i) == 0) return (size >> (i - 4)) + (i - 9) * 8;
  }
  return -1;
}

template <typename BlockHeader>
struct _MemManager {
  typedef BlockHeader Base;
  using bsize_t = typename BlockHeader::bsize_t;
  using block_ptr = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  typedef _MemManager<BlockHeader> MemManager;
  typedef typename BlockHeader::template Pointer<MemManager> ptr;

  const static size_t PAGE_SIZE = 4 * K;
  const static size_t AREA_SIZE = 256 * K;

  offset_t end_area;
  offset_t next_free;

  offset_t end4k;
  offset_t next4k;

  typedef _GarbageSlot<BlockHeader> Slot;
  using GarbageContainer = typename Slot::GarbageContainer;

  typedef _SparseArray<Slot, padding(BLOCK_COUNT, 64)> SlotArray;
  const static size_t EXTRA_SPACE =
      SlotArray::space(BLOCK_COUNT) - sizeof(SlotArray);

  SlotArray slots;

  // the space needed for MemManager
  bsize_t extra_space() const { return slots.space() - sizeof(slots); }
  constexpr static bsize_t extra_space(int slots) {
    return SlotArray::space(slots) - sizeof(SlotArray);
  }

  // init the memory, header is a reserve memory space
  void init(size_t header) {
    header = padding(header, 32);
    next4k = header + GarbageContainer::SIZE;
    next_free = end4k = padding(next4k+1, PAGE_SIZE);
    end_area = AREA_SIZE;
    slots.init();
    int six = assign_block(GarbageContainer::SIZE);
    slots.insert(six, Slot{.ostart = header,
                           .oend = header,
                           .istart = 0,
                           .iend = 0,
                           .count = 0});
  }

  template <typename Storage>
  block_ptr alloc(bsize_t space, Storage& storage) {
    int six = assign_block(space);
    if (six < 0) return nullptr;

    bsize_t bsize = BLOCK_SIZES[six];
    block_ptr result;
    if (slots.get(six)) {
      result = _get_from_slot(six, storage);
      if (result) {
        // Block size is maybe wrong (see the next if + rollback)
        // but the dump is right
        result->block_size = bsize;
        return result;
      }
    }

    if (bsize != GarbageContainer::SIZE) {
#if 0      
      int esix = six < 16 ? 20 : six + 10;
      for (int i = 0; i < 5; i++) {
        six = slots.bits.next(six);
        if (six < 0 || esix < six) break;

        result = _get_from_slot(six, storage);
        if (result) {
          // bsize must be a multiple of the current multiplier
          bsize = padding(bsize, BLOCK_SIZES[six] - BLOCK_SIZES[six - 1]);

          // split the block and take only as much space as needed
          bsize_t waste_block_size = BLOCK_SIZES[six] - bsize;
          if (waste_block_size) {
            block_ptr waste((char*)result + bsize);
            waste->block_size = waste_block_size;
            free(waste, storage);
          }
          result->block_size = bsize;
          return result;
        }
      }
#endif

      if (bsize < PAGE_SIZE) {
        while (next4k + bsize > end4k && next4k < end4k) {
          block_ptr waste = storage.resolve(next4k);
          waste->block_size = end4k - next4k >= 512 ? 512 : end4k - next4k;
          next4k += waste->block_size;
          free(waste, storage);
          // free can cause a 512 alloc!
        }
        _claim_page(storage);
        assert(next4k + bsize <= end4k);
        result = storage.resolve(next4k);
        result->block_size = bsize;
        next4k += bsize;
        return result;
      }
    }

    if (next_free + bsize > end_area) {
      end_area = storage.alloc_area(AREA_SIZE) + AREA_SIZE;
    }
    assert(next_free + PAGE_SIZE <= end_area);

    result = storage.resolve(next_free);
    result->block_size = bsize;
    next_free += bsize;

    size_t next_page = padding(next_free, PAGE_SIZE);
    assert(next_page <= end_area);
    if (next_page > next_free) {
      block_ptr waste = storage.resolve(next_free);
      waste->block_size = next_page - next_free;
      int six = assign_block(waste->block_size);
      assert(BLOCK_SIZES[six] == waste->block_size);
      next_free = next_page;
      _make_slot(six);
      free(waste, storage);
    }

    return result;
  }

  template <typename Storage>
  bool free(const block_ptr& block, Storage& storage) {
    auto six = assign_block(block->block_size);
    if (six < 0) return false;
    assert(block->block_size == BLOCK_SIZES[six]);
    _make_slot(six);
    slots[six].push(block, storage);
    return true;
  }

  void reset() {
    next_free = end_area = end4k = next4k = 0;
    slots.init();
  }

  template <typename Storage>
  block_ptr _get_from_slot(int six, Storage& storage) {
    Slot& slot = slots[six];
    block_ptr result = slot.pop(storage);
    if (slot.ostart == 0) slots.remove(six);
    return result;
  }

  void _make_slot(int six) {
    if (slots.get(six)) return;
    slots.insert(
        six, Slot{.ostart = 0, .oend = 0, .istart = 0, .iend = 0, .count = 0});
  }

  template <typename Storage>
  void _claim_page(Storage& storage) {
    if (next4k == end4k) {
      if (next_free + PAGE_SIZE > end_area) {
        assert(next_free >= end_area);
        end_area = storage.alloc_area(AREA_SIZE) + AREA_SIZE;
      }
      next4k = next_free;
      next_free = end4k = next4k + PAGE_SIZE;
    }
  }
};

struct MemStatistics {
  const static size_t END_IDX = BLOCK_COUNT;

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

  void add(size_t block_size, size_t count, size_t free = 0) {
    int six = assign_block(block_size);
    if (six < 0) return;

    if (!slots.get(six)) {
      slots.insert(
          six, Slot{.block_size = block_size, .count = count, .free = free});
    } else {
      Slot& slot = slots[six];
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
