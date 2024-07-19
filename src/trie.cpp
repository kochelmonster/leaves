#include <algorithm>
#include <cstddef>

#include "memory.hpp"

namespace leaves {

INLINE ssize_t TrieBlock::alloc(ssize_t size) {
  assert(size & 7 == 0); // Must be a multiple of 8
  int id = block_id(size);
  ssize_t result;
  for(; id < 4; id++) {
    ssize_t *next = &free_blocks[id];
    // regain space
    while(*next) {
      FreeBlock* block = (FreeBlock*)(&data[*next]);
      if (block->size >= size) {
        result = *next;
        *next = block->next; // remove block

        ssize_t delta = block->size - size;
        if (delta) {
          // add rest of block to other chain
          block = (FreeBlock*)(&data[result+size]);
          next = &free_blocks[block_id(delta)];
          block->size = delta;
          block->next = *next;
          if (*next) {
            block->free = ((FreeBlock*)(&data[*next]))->free + delta;
          }
          else {
            block->free = delta;
          }
          *next = result + size;
        }
        return result;
      }
      next = &block->next;
    }
  }

  if (size + used > DATA_SIZE)
    return 0;

  result = used;
  used += size;
  return result;
}

INLINE void TrieBlock::free(ssize_t offset, ssize_t size) {
  assert(size & 7 == 0); // Must be a multiple of 8
  ssize_t *next = &free_blocks[block_id(size)];

  FreeBlock *new_block = (FreeBlock*)&data[offset];
  new_block->size = size;
  new_block->free = size;
  new_block->next = *next;

  if (*next) {
    FreeBlock *last_block = (FreeBlock*)&data[*next];
    new_block->free += last_block->free;
  }
  *next = offset;
}

}  // namespace leaves