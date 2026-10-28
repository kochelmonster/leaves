#ifndef _LEAVES_SET_OPERATIONS_HPP
#define _LEAVES_SET_OPERATIONS_HPP

#include "merger.hpp"
#include "intern/util/_set_operations.hpp"

namespace leaves {

template <typename DestinationCursor, typename SourceCursor>
bool set_union(const DestinationCursor& destination, const SourceCursor& source) {
  return Merger(destination, source).exec();
}

// Build the intersection in destination. key_size == 0 requires exact-key
// matches; a nonzero key_size matches by that many leading bytes and retains
// the matching full keys from first. Result values are empty.
template <typename DestinationCursor, typename FirstCursor, typename SecondCursor>
bool intersect(const DestinationCursor& destination, const FirstCursor& first,
               const SecondCursor& second, size_t key_size) {
  _SetOperationObserver observer;
  return _SetOperations::intersect(destination, first, second, key_size, observer);
}

}

#endif