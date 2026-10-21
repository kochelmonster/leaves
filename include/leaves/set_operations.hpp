#ifndef _LEAVES_SET_OPERATIONS_HPP
#define _LEAVES_SET_OPERATIONS_HPP

#include "merger.hpp"
#include "intern/util/_set_operations.hpp"

namespace leaves {

template <typename DestinationCursor, typename SourceCursor>
bool set_union(const DestinationCursor& destination, const SourceCursor& source) {
  return Merger(destination, source).exec();
}

template <typename DestinationCursor, typename FirstCursor, typename SecondCursor>
bool intersect(const DestinationCursor& destination, const FirstCursor& first,
               const SecondCursor& second, size_t key_size) {
  _SetOperationObserver observer;
  return _SetOperations::intersect(destination, first, second, key_size, observer);
}

}

#endif