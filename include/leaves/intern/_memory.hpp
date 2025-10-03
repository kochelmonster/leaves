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

constexpr size_t K = 1024;
constexpr size_t M = 1024 * K;
constexpr size_t G = 1024 * M;
constexpr size_t T = 1024 * G;

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
      // Because of some rollback situations slot_id of result could be wrong
      // but the classification of the slot is right
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

    if (allocation_start + bsize > allocation_end) {
      if (allocation_end - allocation_start > left_over_end - left_over_start) {
        // the smaller left_over is thrown away
        left_over_start = allocation_start;
        left_over_end = allocation_end;
        allocation_start = allocation_end;
      }

      if (allocation_start + bsize > allocation_end) {
        auto area = resolver.alloc_single_area();
        allocation_start = area->content_offset();
        allocation_end = area->end();
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
  Areas are managed through simple linked lists for atomic allocation.
*/

// Area structure that extends AreaSlice with linked list functionality
struct Area : public AreaSlice {
  offset_t next;    // pointer to next area in list (0 if last)
  
  void init(offset_t area_offset, size_t area_size, offset_t next_area = 0) {
    set_offset(area_offset);
    set_size(area_size);
    // Don't modify reference count - it's managed separately
    next = next_area;
  }
  
  // Returns the offset where content can start (after the Area header)
  offset_t content_offset() const {
    return get_offset() + sizeof(Area);
  }
};

static_assert(alignof(Area) >= alignof(void*), "Area alignment must be pointer-aligned");

// Simple linked list of areas with atomic value switching
struct AreaList {
  static constexpr uint8_t MAX_ITER = 10;

  // Double buffered pointers
  offset_t head[2];  // two sets of head pointers
  offset_t tail[2];  // two sets of tail pointers
  uint8_t active;    // 0 or 1 - determines which set is active
  
  void init() {
    head[0] = head[1] = 0;
    tail[0] = tail[1] = 0;
    active = 0;
  }
  
  // Get current active pointers
  offset_t get_head() const { return head[active]; }
  offset_t get_tail() const { return tail[active]; }
  
  // Prepare inactive buffer and atomically switch
  void atomic_switch(offset_t new_head, offset_t new_tail) {
    uint8_t inactive = 1 - active;
    head[inactive] = new_head;
    tail[inactive] = new_tail;
    active = inactive;  // Atomic: single byte write
  }
  
  template <typename Resolver>
  typename Resolver::Traits::template Pointer<Area> pop(Resolver& resolver) {
    if (!get_head()) {
      return nullptr; // empty list
    }
    
    typedef typename Resolver::Traits::template Pointer<Area> area_ptr;
    area_ptr area = resolver.resolve(get_head(), WRITE);
    
    offset_t new_head = area->next;
    offset_t new_tail = new_head ? get_tail() : offset_t(0);
    
    atomic_switch(new_head, new_tail);
    return area;
  }
  
  template <typename Resolver>
  void push(const AreaSlice& area_slice, Resolver& resolver) {
    typedef typename Resolver::Traits::template Pointer<Area> area_ptr;
    
    // Use the area itself to store the Area header
    area_ptr area = resolver.resolve(area_slice.get_offset(), WRITE);
    area->init(area_slice.get_offset(), area_slice.get_size(), get_head());
    resolver.make_dirty(area);
    
    offset_t new_head = area_slice.get_offset();
    offset_t new_tail = get_tail() ? get_tail() : new_head;
    
    atomic_switch(new_head, new_tail);
  }
  
  template <typename Resolver>
  typename Resolver::Traits::template Pointer<Area> find_and_remove(size_t size, Resolver& resolver) {
    if (!get_head()) {
      return nullptr; // empty list
    }
    
    typedef typename Resolver::Traits::template Pointer<Area> area_ptr;
    offset_t prev_offset = 0;
    offset_t curr_offset = get_head();
    uint8_t iter_count = 0;
    
    while (curr_offset && iter_count < MAX_ITER) {
      area_ptr curr = resolver.resolve(curr_offset, WRITE);
      
      if (curr->get_size() >= size) {
        // Found suitable area - remove from list
        offset_t new_head = get_head();
        offset_t new_tail = get_tail();
        
        if (prev_offset) {
          area_ptr prev = resolver.resolve(prev_offset, WRITE);
          prev->next = curr->next;
          resolver.make_dirty(prev);
          // Update tail if we're removing the last element
          if (curr_offset == get_tail()) {
            new_tail = prev_offset;
          }
        } else {
          new_head = curr->next;
          // Update tail if we're removing the only element
          if (curr_offset == get_tail()) {
            new_tail = 0;
          }
        }
        
        // If area is larger than needed, split it
        if (curr->get_size() > size) {
          // Create rest area from the remaining space
          offset_t rest_offset = curr->get_offset() + size;
          size_t rest_size = curr->get_size() - size;
          
          // Insert rest area at head
          area_ptr rest = resolver.resolve(rest_offset, WRITE);
          rest->init(rest_offset, rest_size, new_head);
          new_head = rest_offset;
          resolver.make_dirty(rest);
          
          // Update current area to requested size
          curr->set_size(size);
          resolver.make_dirty(curr);
        }
        
        atomic_switch(new_head, new_tail);
        return curr;
      }
      
      prev_offset = curr_offset;
      curr_offset = curr->next;
      iter_count++;
    }
    
    return nullptr; // No suitable area found
  }
  
  template <typename Resolver>
  void move(AreaList& other, Resolver& resolver) {
    if (!other.get_head()) {
      return; // Nothing to add
    }
    
    typedef typename Resolver::Traits::template Pointer<Area> area_ptr;

    // Phase 1: Clear source first to avoid double ownership
    offset_t other_head = other.get_head();
    offset_t other_tail = other.get_tail();
    other.atomic_switch(0, 0);

    if (!get_head()) {
      // Phase 2: Atomically take ownership
      atomic_switch(other_head, other_tail);
    } else {
      // Phase 2: Connect our tail to other's head (safe now - no double ownership)
      area_ptr tail_area = resolver.resolve(get_tail(), WRITE);
      tail_area->next = other_head;
      resolver.make_dirty(tail_area);
      
      // Phase 3: Atomically update our pointers
      atomic_switch(get_head(), other_tail);
    }
  }
};

// Area pool that handles both single and multi areas
struct AreaPool {
  AreaList single_areas;  // single AREA_SIZE areas
  AreaList multi_areas;   // multi-AREA_SIZE areas
  
  void init() {
    single_areas.init();
    multi_areas.init();
  }
  
  template <typename Resolver>
  typename Resolver::Traits::template Pointer<Area> alloc_single_area(Resolver& resolver) {
    // First try to get from single_areas
    auto area = single_areas.pop(resolver);
    if (area) {
      return area;
    }
    
    // If no single area available, try to get from multi_areas
    // Look for an area that's exactly AREA_SIZE or can be split
    constexpr auto AREA_SIZE = Resolver::Traits::AREA_SIZE;
    area = multi_areas.find_and_remove(AREA_SIZE, resolver);
    return area;
  }
  
  template <typename Resolver>
  typename Resolver::Traits::template Pointer<Area> alloc_multi_area(size_t size, Resolver& resolver) {
    return multi_areas.find_and_remove(size, resolver);
  }
  
  template <typename Resolver>
  void return_single_areas(AreaList& areas, Resolver& resolver) {
    single_areas.move(areas, resolver);
  }
  
  template <typename Resolver>
  void return_multi_areas(AreaList& areas, Resolver& resolver) {
    multi_areas.move(areas, resolver);
  }
};

// Compatibility structure for old tests
struct AreaRegister {
  static constexpr int COUNT = 4;  // Arbitrary constant for test compatibility
  offset_t next;
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
