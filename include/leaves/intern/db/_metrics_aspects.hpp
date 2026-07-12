#ifndef _LEAVES_METRICS_ASPECTS_HPP
#define _LEAVES_METRICS_ASPECTS_HPP

#include <atomic>

#include "_aspect.hpp"

namespace leaves {

// =============================================================================
// Snapshot structs — plain (non-atomic) point-in-time copies of counters.
// =============================================================================

struct OperationSnapshot {
  uint64_t writes = 0;
  uint64_t bytes_written = 0;
  uint64_t reads = 0;
  uint64_t bytes_read = 0;
  uint64_t deletes = 0;
};

struct TransactionSnapshot {
  uint64_t user_txns_started = 0;
  uint64_t user_txns_committed = 0;
  uint64_t merge_txns_committed = 0;
  uint64_t defrag_txns_committed = 0;
  uint64_t user_txns_rolled_back = 0;
};

struct NavigationSnapshot {
  uint64_t finds = 0;
  uint64_t finds_hit = 0;
  uint64_t navigations = 0;
};

struct MaintenanceSnapshot {
  uint64_t sanitize_count = 0;
  uint64_t defrag_count = 0;
  uint64_t reset_count = 0;
};

struct MergeSnapshot {
  uint64_t merge_overwrites = 0;
  uint64_t merge_adds = 0;
  uint64_t merge_deletes = 0;
};

// =============================================================================
// _OperationAspect — counts write, read, and delete operations.
//
// Hooks: on_write, on_read, may_delete
// Snapshot: ops_snapshot() -> OperationSnapshot
// =============================================================================

template <typename Base = DefaultAspect>
struct _OperationAspect : Base {
  struct CursorContext : Base::CursorContext {};

  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> deletes{0};

  Slice on_write(const Slice& key, const Slice& value, CursorContext& ctx) {
    Slice result = Base::on_write(key, value, ctx);
    writes.fetch_add(1, std::memory_order_relaxed);
    bytes_written.fetch_add(result.size(), std::memory_order_relaxed);
    return result;
  }

  Slice on_read(const Slice& key, const Slice& data, const Slice& big_meta,
                CursorContext& ctx) {
    Slice result = Base::on_read(key, data, big_meta, ctx);
    reads.fetch_add(1, std::memory_order_relaxed);
    bytes_read.fetch_add(result.size(), std::memory_order_relaxed);
    return result;
  }

  bool may_delete(const Slice& key, const Slice& value, CursorContext& ctx) {
    if (!Base::may_delete(key, value, ctx)) return false;
    deletes.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  OperationSnapshot ops_snapshot() const {
    return {
        writes.load(std::memory_order_relaxed),
        bytes_written.load(std::memory_order_relaxed),
        reads.load(std::memory_order_relaxed),
        bytes_read.load(std::memory_order_relaxed),
        deletes.load(std::memory_order_relaxed),
    };
  }
};

// =============================================================================
// _TransactionAspect — counts transaction lifecycle events by origin.
//
// Hooks: on_start_transaction, on_commit, on_rollback
// Snapshot: txn_snapshot() -> TransactionSnapshot
// =============================================================================

template <typename Base = DefaultAspect>
struct _TransactionAspect : Base {
  struct CursorContext : Base::CursorContext {};

  std::atomic<uint64_t> user_txns_started{0};
  std::atomic<uint64_t> user_txns_committed{0};
  std::atomic<uint64_t> merge_txns_committed{0};
  std::atomic<uint64_t> defrag_txns_committed{0};
  std::atomic<uint64_t> user_txns_rolled_back{0};

  template <typename DB>
  void on_start_transaction(DB& db, tid_t tid, TransactionOrigin origin,
                            CursorContext& ctx) {
    Base::on_start_transaction(db, tid, origin, ctx);
    if (origin == TransactionOrigin::user)
      user_txns_started.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename DB>
  void on_commit(DB& db, TransactionOrigin origin, CursorContext& ctx) {
    Base::on_commit(db, origin, ctx);
    switch (origin) {
      case TransactionOrigin::user:
        user_txns_committed.fetch_add(1, std::memory_order_relaxed);
        break;
      case TransactionOrigin::merge:
        merge_txns_committed.fetch_add(1, std::memory_order_relaxed);
        break;
      case TransactionOrigin::defrag:
        defrag_txns_committed.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }

  template <typename DB>
  void on_rollback(DB& db, tid_t tid, TransactionOrigin origin,
                   CursorContext& ctx) {
    Base::on_rollback(db, tid, origin, ctx);
    if (origin == TransactionOrigin::user)
      user_txns_rolled_back.fetch_add(1, std::memory_order_relaxed);
  }

  TransactionSnapshot txn_snapshot() const {
    return {
        user_txns_started.load(std::memory_order_relaxed),
        user_txns_committed.load(std::memory_order_relaxed),
        merge_txns_committed.load(std::memory_order_relaxed),
        defrag_txns_committed.load(std::memory_order_relaxed),
        user_txns_rolled_back.load(std::memory_order_relaxed),
    };
  }
};

// =============================================================================
// _NavigationAspect — counts find, next, and prev calls.
//
// Hooks: on_find, on_next, on_prev
// Snapshot: nav_snapshot() -> NavigationSnapshot
// =============================================================================

template <typename Base = DefaultAspect>
struct _NavigationAspect : Base {
  struct CursorContext : Base::CursorContext {};

  std::atomic<uint64_t> finds{0};
  std::atomic<uint64_t> finds_hit{0};
  std::atomic<uint64_t> navigations{0};

  void on_find(const Slice& key, bool found, CursorContext& ctx) {
    Base::on_find(key, found, ctx);
    finds.fetch_add(1, std::memory_order_relaxed);
    if (found) finds_hit.fetch_add(1, std::memory_order_relaxed);
  }

  void on_next(bool has_next, CursorContext& ctx) {
    Base::on_next(has_next, ctx);
    navigations.fetch_add(1, std::memory_order_relaxed);
  }

  void on_prev(bool has_prev, CursorContext& ctx) {
    Base::on_prev(has_prev, ctx);
    navigations.fetch_add(1, std::memory_order_relaxed);
  }

  NavigationSnapshot nav_snapshot() const {
    return {
        finds.load(std::memory_order_relaxed),
        finds_hit.load(std::memory_order_relaxed),
        navigations.load(std::memory_order_relaxed),
    };
  }
};

// =============================================================================
// _MaintenanceAspect — counts sanitize, defrag, and reset operations.
//
// Hooks: on_sanitize, on_defrag, on_reset
// Snapshot: maintenance_snapshot() -> MaintenanceSnapshot
// =============================================================================

template <typename Base = DefaultAspect>
struct _MaintenanceAspect : Base {
  struct CursorContext : Base::CursorContext {};

  std::atomic<uint64_t> sanitize_count{0};
  std::atomic<uint64_t> defrag_count{0};
  std::atomic<uint64_t> reset_count{0};

  template <typename DB>
  void on_sanitize(DB& db) {
    Base::on_sanitize(db);
    sanitize_count.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename DB>
  void on_defrag(DB& db) {
    Base::on_defrag(db);
    defrag_count.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename DB>
  void on_reset(DB& db) {
    Base::on_reset(db);
    reset_count.fetch_add(1, std::memory_order_relaxed);
  }

  MaintenanceSnapshot maintenance_snapshot() const {
    return {
        sanitize_count.load(std::memory_order_relaxed),
        defrag_count.load(std::memory_order_relaxed),
        reset_count.load(std::memory_order_relaxed),
    };
  }
};

// =============================================================================
// _MergeAspect — counts merge operations applied during replication.
//
// Hooks: may_merge_overwrite, may_merge_add, may_merge_delete
// Snapshot: merge_snapshot() -> MergeSnapshot
// =============================================================================

template <typename Base = DefaultAspect>
struct _MergeAspect : Base {
  struct CursorContext : Base::CursorContext {};

  std::atomic<uint64_t> merge_overwrites{0};
  std::atomic<uint64_t> merge_adds{0};
  std::atomic<uint64_t> merge_deletes{0};

  bool may_merge_overwrite(const Slice& key, const Slice& dst, bool dst_is_big,
                           const Slice& src, bool src_is_big,
                           CursorContext& ctx) {
    if (!Base::may_merge_overwrite(key, dst, dst_is_big, src, src_is_big, ctx))
      return false;
    merge_overwrites.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool may_merge_add(const Slice& key, const Slice& value, bool is_big,
                     CursorContext& ctx) {
    if (!Base::may_merge_add(key, value, is_big, ctx)) return false;
    merge_adds.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  bool may_merge_delete(const Slice& key, const Slice& meta,
                        CursorContext& ctx) {
    if (!Base::may_merge_delete(key, meta, ctx)) return false;
    merge_deletes.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  MergeSnapshot merge_snapshot() const {
    return {
        merge_overwrites.load(std::memory_order_relaxed),
        merge_adds.load(std::memory_order_relaxed),
        merge_deletes.load(std::memory_order_relaxed),
    };
  }
};

// =============================================================================
// _AllMetricsAspect — convenience alias stacking all five metric mixins.
//
// Usage:
//   struct MyTraits : _MemoryMapTraits { using Aspect = _AllMetricsAspect<>; };
//
// Selective composition:
//   using Aspect = _TransactionAspect<_OperationAspect<DefaultAspect>>;
//
// Chaining with a custom aspect:
//   using Aspect = _AllMetricsAspect<MyFilterAspect<>>;
// =============================================================================

template <typename Base = DefaultAspect>
using _AllMetricsAspect = _TransactionAspect<_OperationAspect<
    _NavigationAspect<_MaintenanceAspect<_MergeAspect<Base>>>>>;

}  // namespace leaves

#endif  // _LEAVES_METRICS_ASPECTS_HPP
