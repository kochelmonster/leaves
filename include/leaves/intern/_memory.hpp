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

const static size_t PAGE_SIZE = 4 * K;
const static size_t AREA_SIZE = 1 * M;


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
        assert(new_back->block_size == GarbageContainer::SIZE);
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

  offset_t set_end(offset_t next_free, bsize_t bsize) {
    assert(next_free % 4096 == 0);
    end_free = next_free + ((bsize < 1 * K) ? PAGE_SIZE : 8 * bsize);
    return end_free;
  }

  offset_t next_free;  // the next in the free area
  offset_t end_free;   // end of free area
  offset_t ostart;     // offset of the start
  offset_t oend;       // offset of the end
  uint16_t istart;     // index inside the start
  uint16_t iend;       // index inside the end
  bsize_t count;
};

static constexpr size_t BLOCK_SIZES[] = {
    64,    80,    128,   288,   512,   1024,  1536,  2048,  2560,
    3072,  3584,  4096,  4608,  5120,  5632,  6144,  6656,  7168,
    7680,  8192,  12288, 16384, 20480, 24576, 28672, 32768, 36864,
    40960, 45056, 49152, 53248, 57344, 61440, 65536};

static const size_t BLOCK_COUNT = sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

static const size_t MIN_BLOCK = BLOCK_SIZES[0];

inline constexpr int assign_block(size_t size) {
  assert(size > 0);
  if (size <= 128) {
    if (size <= 64) return 0;
    if (size <= 80) return 1;
    return 2;
  }
  if (size <= 288) return 3;
  if (size <= 7680) return (size - 1) / 512 + 4;
  int result = (size - 1) / 4096 + 18;
  return result < BLOCK_COUNT ? result : -1;
}

template <typename BlockHeader>
struct _MemManager {
  typedef BlockHeader Base;
  using bsize_t = typename BlockHeader::bsize_t;
  using block_ptr = typename BlockHeader::ptr;
  using offset_t = typename BlockHeader::offset_t;
  typedef _MemManager<BlockHeader> MemManager;
  typedef typename BlockHeader::template Pointer<MemManager> ptr;

  typedef _GarbageSlot<BlockHeader> Slot;
  using GarbageContainer = typename Slot::GarbageContainer;

  typedef _SparseArray<Slot, padding(BLOCK_COUNT, 64)> SlotArray;
  const static size_t EXTRA_SPACE =
      SlotArray::space(BLOCK_COUNT) - sizeof(SlotArray);

  offset_t allocation_end;
  offset_t next_free;
  SlotArray slots;

  // the space needed for MemManager
  bsize_t extra_space() const { return slots.space() - sizeof(slots); }
  constexpr static bsize_t extra_space(int slots) {
    return SlotArray::space(slots) - sizeof(SlotArray);
  }

  // init the memory, header is a reserve memory space
  void init(size_t header) {
    header = padding(header, MIN_BLOCK);
    next_free = padding(header, PAGE_SIZE);
    slots.init();
    slots.insert(0, Slot{.next_free = header,
                         .end_free = next_free,
                         .ostart = 0,
                         .oend = 0,
                         .istart = 0,
                         .iend = 0,
                         .count = 0});
    allocation_end = AREA_SIZE;
    assert(allocation_end == padding(next_free, AREA_SIZE));
  }

  template <typename Storage>
  void set_next_free(offset_t next, Storage& storage) {
    next_free = next;
    if (next_free >= allocation_end) {
      allocation_end += AREA_SIZE;
      assert(allocation_end > next_free);
      storage.extend_file(allocation_end);
    }
  }

  template <typename Storage>
  block_ptr alloc(bsize_t space, Storage& storage) {
    int six = assign_block(space);
    if (six < 0) return nullptr;

    bsize_t bsize = BLOCK_SIZES[six];

    _make_slot(six);
    Slot& slot = slots[six];
    block_ptr result = slot.pop(storage);
    if (result) {
      // Block size is maybe wrong (see the next if + rollback)
      // but the dump is right
      result->block_size = bsize;
      return result;
    }

    if (slot.next_free + bsize > slot.end_free) {
      assert(slot.next_free % PAGE_SIZE == 0);
      assert(slot.end_free % PAGE_SIZE == 0);
      assert(next_free % PAGE_SIZE == 0);
      assert(slot.next_free == slot.end_free);

      if (six == 1) {
        offset_t start = next_free;
        slot.next_free = start + 4 * MIN_BLOCK;
        assert((slot.next_free + 48 * bsize) % PAGE_SIZE == 0);
        set_next_free(slot.set_end(next_free, bsize), storage);
        for (int i = 0; i < 4; i++) {
          block_ptr p = storage.resolve(start);
          p->block_size = MIN_BLOCK;
          slots[0].push(p, storage);
          start += MIN_BLOCK;
        }
      } else if (six == 3) {
        block_ptr p = storage.resolve(next_free);
        slot.next_free = next_free + MIN_BLOCK;
        assert((slot.next_free + 14 * bsize) % PAGE_SIZE == 0);
        set_next_free(slot.set_end(next_free, bsize), storage);
        p->block_size = MIN_BLOCK;
        slots[0].push(p, storage);
      } else {
        slot.next_free = next_free;
        set_next_free(slot.set_end(next_free, bsize), storage);
      }
    }

    result = storage.resolve(slot.next_free);
    slot.next_free += bsize;
    result->block_size = bsize;
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

  void _make_slot(int six) {
    if (slots.get(six)) return;
    slots.insert(six, Slot{.next_free = 0,
                           .end_free = 0,
                           .ostart = 0,
                           .oend = 0,
                           .istart = 0,
                           .iend = 0,
                           .count = 0});
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
