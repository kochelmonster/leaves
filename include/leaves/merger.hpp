#ifndef _LEAVES_PUBLIC_MERGER_HPP
#define _LEAVES_PUBLIC_MERGER_HPP

#include <type_traits>

#include "cursor.hpp"
#include "intern/util/_merger.hpp"

namespace leaves {

template <typename DestinationCursor, typename SourceCursor>
class Merger {
  static_assert(
      std::is_same_v<typename DestinationCursor::DBImpl,
                     _DB<typename DestinationCursor::StorageImpl>> &&
          std::is_same_v<typename SourceCursor::DBImpl,
                         _DB<typename SourceCursor::StorageImpl>>,
      "Merger requires ordinary DB cursors; replication and Confluence "
      "cursors are not supported.");

 public:
  Merger(const DestinationCursor& destination, const SourceCursor& source)
      : _destination(destination), _source(source) {}

  bool exec() {
    StandardMergePolicy policy;
    return exec(policy);
  }

  template <typename MergePolicy>
  bool exec(MergePolicy& policy) {
    if (!_destination._cursor || !_source._cursor) return false;

    auto& destination = *_destination._cursor;
    auto& source = *_source._cursor;
    if (!destination.is_transaction_active() || destination._use_wal ||
        source.is_transaction_active() ||
        static_cast<const void*>(destination._db) ==
            static_cast<const void*>(source._db)) {
      return false;
    }

    _reset_destination();
    _NavigationReset reset{*this};
    using DstCursor = typename DestinationCursor::cursor_ptr::element_type;
    using SrcCursorImpl = typename SourceCursor::cursor_ptr::element_type;
    using SrcCursor = _Cursor<typename SrcCursorImpl::Traits>;
    SrcCursor traversal(source._db, source._root);
    traversal.clear();
    _Merger<DstCursor, SrcCursor, MergePolicy>(destination, traversal, policy)
        .exec();
    return true;
  }

 private:
  struct _NavigationReset {
    Merger& merger;
    ~_NavigationReset() { merger._reset_destination(); }
  };

  void _reset_destination() noexcept {
    auto& destination = *_destination._cursor;
    destination.stack.clear();
    destination.rest_key.reset();
    destination.current_key.clear();
  }

  DestinationCursor _destination;
  SourceCursor _source;
};

}  // namespace leaves

#endif  // _LEAVES_PUBLIC_MERGER_HPP