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

const static size_t AREA_SIZE = 1 * M;

constexpr uint16_t binary_search(const uint16_t* first, const uint16_t* last,
                                 uint16_t value) {
  const uint16_t *mid = first, *start = first;
  while (first < last) {
    mid = first + (last - first) / 2;
    if (*mid == value) return mid - start;
    if (*mid < value)
      first = mid + 1;
    else
      last = mid;
  }
  return first - start;
}

// One page inside the garbage queue
template <typename Traits>
struct _GarbageContainer : public Traits::BlockHeader {
  using offset_e = typename Traits::offset_e;
  using tid_e = typename Traits::tid_e;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static const uint16_t SIZE = 4096;
  static const uint16_t SLOT_ID =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]) - 1;
  typedef typename Traits::BlockHeader Base;
  typedef typename Traits::template Pointer<_GarbageContainer> ptr;

  struct FreeBlock {
    offset_e link;  // link to the page
    tid_e txn_id;   // the transaction that freed the page
  };

  static const size_t COUNT =
      (SIZE - sizeof(offset_e) - sizeof(Base)) / sizeof(FreeBlock);

  offset_e next;
  FreeBlock blocks[COUNT];
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

template <typename Traits>
struct _GarbageSlot {
    using BlockHeader = typename Traits::BlockHeader;
  using offset_e = typename Traits::offset_e;
  using uint16_e = typename Traits::uint16_e;
  using uint64_e = typename Traits::uint64_e;
  using ptr = typename Traits::ptr;
  typedef _GarbageSlot<Traits> GarbageSlot;
  typedef _GarbageContainer<Traits> GarbageContainer;
  using garb_ptr = typename GarbageContainer::ptr;

  template <typename Storage>
  ptr pop(Storage& storage) {
    if (count == 0) return nullptr;

    assert(!(ostart == oend && istart == iend));

    garb_ptr front(storage.resolve(ostart));
    if (!storage.template may_recycle(front->blocks[istart])) return nullptr;

    assert(front->blocks[istart].link != 0);
    ptr result = storage.resolve(front->blocks[istart].link);
    count--;
    istart++;

    if (ostart == oend && istart >= iend) {
      assert(count == 0);
      if (result->slot_id != GarbageContainer::SLOT_ID) {
        istart = iend = 0;
        ostart = oend = 0;
        storage.free(front);
      } else {
        istart = iend = 0;
      }
    } else if (istart >= GarbageContainer::COUNT) {
      ostart = front->next;
      istart = 0;
      storage.free(front);
    }
    return result;
  }

  template <typename Storage>
  void push(ptr& block, Storage& storage) {
    garb_ptr back;

    assert(oend != 1);  // locked

    if (oend) {
      back = storage.resolve(oend);
      if (iend >= GarbageContainer::COUNT) {
        garb_ptr new_back = storage.alloc_slot(GarbageContainer::SLOT_ID);
        assert(new_back->slot_id == GarbageContainer::SLOT_ID);
        oend = back->next = storage.resolve(new_back);
        iend = 0;
        back = new_back;
      }
    } else {
      assert(ostart == 0);
      assert(istart == 0);
      assert(iend == 0);
      assert(count == 0);
      back = storage.alloc_slot(GarbageContainer::SLOT_ID);
      oend = ostart = storage.resolve(back);
    }
    back->blocks[iend].link = storage.resolve(block);
    block->free_idx = iend;
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

      for (; index < GarbageContainer::COUNT; index++)
        call(container->blocks[index]);

      index = 0;
      ocontainer = container->next;
    }
  }

  offset_e next_free;  // the next in the free area
  offset_e end_free;   // end of free area
  offset_e ostart;     // offset of the start
  offset_e oend;       // offset of the end
  uint64_e count;      // count of freed blocks
  uint16_e istart;     // index inside the start
  uint16_e iend;       // index inside the end
};

template <typename Traits>
struct _MemManager {
  using uint32_e = typename Traits::uint32_e;
  using offset_e = typename Traits::offset_e;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  typedef typename Traits::BlockHeader BlockHeader;
  typedef _MemManager<Traits> MemManager;
  using ptr = typename Traits::Pointer<MemManager>;
  using block_ptr = typename Traits::ptr;

  typedef _GarbageSlot<Traits> Slot;
  using GarbageContainer = typename Slot::GarbageContainer;

  static constexpr uint16_t BLOCK_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
  static constexpr uint16_t PAGE_ID = BLOCK_COUNT - 1;
  static constexpr uint16_t MIN_BLOCK_SIZE = BLOCK_SIZES[0];

  offset_e allocation_end;
  offset_e next_free;
  Slot slots[BLOCK_COUNT];

  // init the memory, header is a reserve memory space
  void init(uint16_t header) {
    header = padding(header, PAGE_SIZE);
    next_free = padding(header, PAGE_SIZE);
    memset(slots, 0, sizeof(slots));
    allocation_end = AREA_SIZE;
    assert(allocation_end == padding(next_free, AREA_SIZE));
  }

  static constexpr int assign_slot(uint16_t size) {
    assert(size > 0);
    return binary_search(&BLOCK_SIZES[0], &BLOCK_SIZES[BLOCK_COUNT], size);
  }

  template <typename Storage>
  void set_next_free(offset_e next, Storage& storage) {
    assert(next % PAGE_SIZE == 0);
    next_free = next;
    if (next_free >= allocation_end) {
      allocation_end += AREA_SIZE;
      assert(allocation_end > next_free);
      storage.extend_file(allocation_end);
    }
  }

  template <typename Storage>
  block_ptr alloc(uint8_t sidx, Storage& storage) {
    uint16_t bsize = BLOCK_SIZES[sidx];

    Slot& slot = slots[sidx];
    block_ptr result = slot.pop(storage);
    if (result) {
      // Block size is maybe wrong (see the next if + rollback)
      // but the dump is right
      result->slot_id = sidx;
      return result;
    }

    if (slot.next_free + bsize > slot.end_free) {
      assert(next_free % PAGE_SIZE == 0);
      assert(slot.next_free == slot.end_free);

      int count = PAGE_SIZE / bsize;
      uint16_t rest_space = PAGE_SIZE - count * bsize;
      assert(rest_space % 8 == 0);

      if (slots[PAGE_ID].count > 10) {
        block_ptr page = slots[PAGE_ID].pop(storage);
        slot.next_free = storage.resolve(page);
      } else {
        set_next_free(next_free + PAGE_SIZE, storage);
        slot.next_free = next_free;
      }
      slot.end_free = slot.next_free + PAGE_SIZE - rest_space;

      offset_t start = slot.end_free;
      for (int id = sidx - 1; id >= 0 && rest_space > MIN_BLOCK_SIZE; id--) {
        uint16_t bs = BLOCK_SIZES[id];
        while (bs < rest_space) {
          block_ptr p = storage.resolve(start);
          slots[id].push(p, storage);
          start += bs;
          rest_space -= bs;
        }
      }
    }

    result = storage.resolve(slot.next_free);
    slot.next_free += bsize;
    result->slot_id = sidx;
    result->free_idx = 0;
    return result;
  }

  template <typename Storage>
  bool free(block_ptr& block, Storage& storage) {
    auto six = block->slot_id;
    slots[six].push(block, storage);
    return true;
  }
};

template <typename Traits>
struct _MemStatistics {
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static const uint16_t SIZE = 4096;
  static const uint16_t BLOCK_COUNT =
      sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);

  struct Slot {
    size_t block_size;
    size_t count;
    size_t free;
  };

  Slot slots[BLOCK_COUNT];

  _MemStatistics() { memset(slots, 0, sizeof(slots)); }

  void add(uint16_t sidx, size_t count, size_t free = 0) {
    Slot& slot = slots[sidx];
    slot.count += count;
    slot.free += free;
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
