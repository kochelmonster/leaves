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
#include <atomic>
#include <cassert>
#include <cstddef>

#include "_bits.hpp"
#include "_util.hpp"

namespace leaves {

const size_t K = 1024;
const size_t M = 1024 * K;
const size_t G = 1024 * M;
const size_t T = 1024 * G;

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

// A 4K Structure to register memory blocks.
template <typename Traits>
struct _BlockContainer : public Traits::BlockHeader {
  using offset_e = typename Traits::offset_e;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr uint16_t SLOT_ID = Traits::BLOCK_SIZES_COUNT - 1;
  static const uint16_t SIZE = 4 * K;
  typedef typename Traits::BlockHeader Base;
  typedef typename Traits::template Pointer<_BlockContainer> ptr;

  struct BlockItem {
    offset_e link;  // link to the page
    tid_t txn_id;   // the transaction that freed the page
  };

  static const size_t COUNT =
      (SIZE - sizeof(offset_e) - sizeof(Base)) / sizeof(BlockItem);

  offset_e next;
  BlockItem blocks[COUNT];
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
  typedef _BlockContainer<Traits> BlockContainer;
  using cont_ptr = typename BlockContainer::ptr;

  template <typename Resolver>
  ptr pop(Resolver& resolver) {
    if (count == 0) return nullptr;

    assert(!(ostart == oend && istart == iend));

    cont_ptr front(resolver.resolve(ostart, WRITE));
    if (!resolver.template may_recycle(front->blocks[istart])) return nullptr;

    assert(front->blocks[istart].link != 0);
    ptr result = resolver.resolve(front->blocks[istart].link, WRITE);

    count--;
    istart++;

    if (ostart == oend && istart >= iend) {
      assert(count == 0);
      if (result->slot_id != BlockContainer::SLOT_ID) {
        istart = iend = 0;
        ostart = oend = 0;
        resolver.free(front);
      } else {
        istart = iend = 0;
      }
    } else if (istart >= BlockContainer::COUNT) {
      ostart = front->next;
      istart = 0;
      resolver.free(front);
    }
    return result;
  }

  template <typename Resolver>
  void push(ptr& block, Resolver& resolver) {
    cont_ptr back;

    if (oend) {
      back = resolver.resolve(oend, WRITE);
      if (iend >= BlockContainer::COUNT) {
        cont_ptr new_back = resolver.alloc_slot(BlockContainer::SLOT_ID);
        assert(new_back->slot_id == BlockContainer::SLOT_ID);
        oend = back->next = resolver.resolve(new_back);
        iend = 0;

        resolver.make_dirty(back);
        back = new_back;
      }
    } else {
      assert(ostart == 0);
      assert(istart == 0);
      assert(iend == 0);
      assert(count == 0);
      back = resolver.alloc_slot(BlockContainer::SLOT_ID);
      oend = ostart = resolver.resolve(back);
    }
    back->blocks[iend].link = resolver.resolve(block);
    block->free_idx = iend;
    assert(back->blocks[iend].link != 0);
    resolver.template mark_for_recycle(back->blocks[iend]);
    resolver.make_dirty(back);

    iend++;
    count++;
  }

  template <typename Caller, typename Resolver>
  void iter(Resolver& resolver, Caller call) {
    offset_t ocontainer = ostart;
    uint16_t index = istart;

    while (true) {
      cont_ptr container = resolver.resolve(ocontainer);

      if (ocontainer == oend) {
        for (; index < iend; index++) {
          call(container->blocks[index]);
        }
        break;
      }

      for (; index < BlockContainer::COUNT; index++)
        call(container->blocks[index]);

      index = 0;
      ocontainer = container->next;
    }
  }

  offset_e ostart;  // offset of the start
  offset_e oend;    // offset of the end
  uint64_e count;   // count of freed blocks
  uint16_e istart;  // index inside the start
  uint16_e iend;    // index inside the end
};

template <typename Traits>
struct _MemManager {
  using uint32_e = typename Traits::uint32_e;
  using offset_e = typename Traits::offset_e;
  static constexpr auto AREA_SIZE = Traits::AREA_SIZE;
  static constexpr auto COUNT = Traits::BLOCK_SIZES_COUNT;
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  typedef typename Traits::BlockHeader BlockHeader;
  typedef _MemManager<Traits> MemManager;
  using ptr = typename Traits::Pointer<MemManager>;
  using block_ptr = typename Traits::ptr;

  typedef _GarbageSlot<Traits> Slot;
  using BlockContainer = typename Slot::BlockContainer;

  static constexpr uint16_t PAGE_ID = COUNT - 1;
  static constexpr uint16_t MIN_BLOCK_SIZE = BLOCK_SIZES[0];
  static constexpr uint16_t MAX_BLOCK_SIZE = BLOCK_SIZES[COUNT - 1];

  offset_e allocation_end;
  offset_e allocation_start;
  offset_e left_over_start;
  offset_e left_over_end;
  Slot slots[COUNT];

  // init the memory, header is a reserve memory space
  void init(offset_t header, offset_t alloction_end_) {
    memset(slots, 0, sizeof(slots));
    allocation_start = header;
    allocation_end = alloction_end_;
    left_over_end = left_over_start = 0;
    assert(allocation_end % AREA_SIZE == 0);
  }

  static constexpr int assign_slot(uint16_t size) {
    assert(size > 0);
    return binary_search(&BLOCK_SIZES[0], &BLOCK_SIZES[COUNT], size);
  }

  template <typename Resolver>
  block_ptr alloc(uint8_t sidx, Resolver& resolver) {
    uint16_t bsize = BLOCK_SIZES[sidx];

    Slot& slot = slots[sidx];
    block_ptr result = slot.pop(resolver);
    if (result) {
      // Block size is maybe wrong (see the next if + rollback)
      // but the dump is right
      result->slot_id = sidx;
      return result;
    }

    if (left_over_start + bsize <= left_over_end) {
      result = resolver.resolve(left_over_start, WRITE);
      left_over_start += bsize;
      result->slot_id = sidx;
      result->free_idx = 0;
      return result;
    }

    offset_t page_border = std::min(padding(allocation_start + 1, AREA_SIZE),
                                    (uint64_t)allocation_end);
    if (allocation_start + bsize > page_border) {
      if (allocation_end - allocation_start > left_over_end - left_over_start) {
        // the smaller left_over is thrown away
        left_over_start = allocation_start;
        left_over_end = page_border;
        allocation_start = page_border;
      }

      if (allocation_start + bsize > allocation_end) {
        auto area = resolver.alloc_page();
        allocation_start = area.get_offset();
        allocation_end = area.end();
        assert(allocation_end % AREA_SIZE == 0);
      }
    }

    result = resolver.resolve(allocation_start, WRITE);
    allocation_start += bsize;
    result->slot_id = sidx;
    result->free_idx = 0;
    return result;
  }

  template <typename Resolver>
  void free(block_ptr& block, Resolver& resolver) {
    slots[block->slot_id].push(block, resolver);
  }
};

template <typename Traits>
struct _MemStatistics {
  static constexpr auto& BLOCK_SIZES = Traits::BLOCK_SIZES;
  static constexpr uint16_t COUNT = Traits::BLOCK_SIZES_COUNT;
  static const uint16_t SIZE = 4096;

  struct Slot {
    size_t block_size;
    size_t count;
    size_t free;
  };

  Slot slots[COUNT];

  _MemStatistics() { memset(slots, 0, sizeof(slots)); }

  void add(uint16_t sidx, size_t count, size_t free = 0) {
    Slot& slot = slots[sidx];
    slot.block_size = BLOCK_SIZES[sidx];
    slot.count += count;
    slot.free += free;
  }
};

/*
  New space from the resolver is allocated in memory chunks of AREA_SIZE.
  These chunks are called areas.
  Every database keeps track of their allocate areas and gives them
  back to resolver for reuse once the database is deleted.

  The data structure to track the areas is a hybrid of a list and a table.
  Certain areas spend their first 4K bytes for a area register (an array).
  If the register is full the next area becomes a new area register and
  the old register points to the new one.

    Area 1(Register1)
  ----------
  | next   |--------------------------->  Area n (Register 2)
  | item1  |---------------->  Area2    ----------
  | item2  |--->  Area 3      -------   | next   |
  |  ..    |     --------     |     |   | item 1 |
  ----------     |      |     |     |   | item 2 |
                 |      |     -------   |  ...   |
                 --------               ----------
*/

struct AreaRegister {
  static const uint16_t SIZE = 4 * K;
  static const size_t COUNT = (SIZE - 2 * sizeof(uint64_t)) / sizeof(AreaSlice);

  AreaSlice areas[COUNT];  // in areas[0] is the date of itself
  offset_t next;
  int64_t last_index;  // last_index of areas (== count-1)

  void init(AreaSlice me) {
    next = 0;
    last_index = 0;
    me.set_offset(me.get_offset() + SIZE);
    me.set_size(me.get_size() - SIZE);
    areas[0] = me;
  }
};

// Manages allocation Areas from Resolver
struct AreaManager {
  offset_t start;  // the offset of the first area register
  offset_t end;    // the offset of the last area register

  template <typename Resolver>
  void merge(AreaManager* other, Resolver& resolver) {
    // insert the register list of other in my list
    if (!other->start) return;
    typedef typename Resolver::Traits::template Pointer<AreaRegister> ptr;
    if (!start) {
      assert(!end);
      start = other->start;
      end = other->end;
    } else {
      assert(end);
      ptr tmp = resolver.resolve(end, WRITE);
      tmp->next = other->start;
      end = other->end;
    }
    assert(((ptr)resolver.resolve(end))->next == 0);
  }

  template <typename Resolver>
  AreaSlice get(size_t min_size, Resolver& resolver) {
    // min_size can also be a multiple of AREA_SIZE
    assert(min_size >= Resolver::Traits::AREA_SIZE);
    assert(min_size % Resolver::Traits::AREA_SIZE == 0);

    typedef typename Resolver::Traits::template Pointer<AreaRegister> ptr;

    // iterate through the structure to find an area that is big enough
    offset_t oiter = start;
    while (oiter) {
      ptr ar = resolver.resolve(oiter, WRITE);

      for (int i = ar->last_index; i >= 1; i--) {
        auto& block = ar->areas[i];
        if (block.get_size() >= min_size) {
          AreaSlice result = block;
          block.set_size(0);  // result has still original size!
          while (ar->last_index && !ar->areas[ar->last_index].get_size())
            ar->last_index--;
          return result;
        }
      }

      if (ar->last_index == 0 &&
          ar->areas[0].get_size() + AreaRegister::SIZE >= min_size) {
        start = ar->next;
        if (start == 0) end = 0;
        // regain the space used for AreaRegister
        return AreaSlice{ar->areas[0].offset - AreaRegister::SIZE,
                         ar->areas[0].get_size() + AreaRegister::SIZE};
      }

      oiter = ar->next;
    }

    return AreaSlice();
  }

  template <typename Resolver>
  void put(AreaSlice& area, Resolver& resolver) {
    typedef typename Resolver::Traits::template Pointer<AreaRegister> ptr;

    assert(area.get_size() > sizeof(AreaRegister));
    assert(area.get_size() >= Resolver::Traits::AREA_SIZE);
    if (!end) {
      // the first area
      assert(start == 0);
      ptr ar = resolver.resolve(area.get_offset(), WRITE);
      ar->init(area);
      start = end = area.get_offset();
      return;
    }

    ptr ar = resolver.resolve(end, WRITE);
    if (ar->last_index < AreaRegister::COUNT - 1) {
      ar->areas[++ar->last_index] = area;
      return;
    }

    // the last register is full -> the area becomes a new register
    end = ar->next = area.get_offset();
    ar = resolver.resolve(area.get_offset(), WRITE);
    ar->init(area);
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
transferable resolver
  -           heao        x         -           -     in memory resolver
*/

#endif  // _LEAVES__MEMORY_HPP
