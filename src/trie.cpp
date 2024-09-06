#include <algorithm>
#include <cstddef>

#include "memory.hpp"

namespace leaves {

INLINE node_ptr TrieBlock::alloc(ssize_t size, NodeType type) {
  assert(type != kNull);
  assert((size & 7) == 0);
  
  ssize_t result;

  if (size + used <= DATA_SIZE) {
    result = used;
    used += size;
    memset(&data[result], 0, size);
    return node_ptr(result >> 3, type);
  }
  
  return node_ptr(0, kNull);
}


}  // namespace leaves