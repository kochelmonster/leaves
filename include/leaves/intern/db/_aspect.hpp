#ifndef _LEAVES_ASPECT_HPP
#define _LEAVES_ASPECT_HPP

#include "../core/_util.hpp"

namespace leaves {

// =============================================================================
// DefaultAspect: No-op aspect with zero overhead
// =============================================================================
// The Aspect provides cross-cutting join points for cursor and merge
// operations.  Users define a custom Aspect struct in their Traits to
// intercept writes, reads, deletes, and merge decisions.
//
// The Aspect type is detected from Traits via SFINAE (Traits::Aspect).
// When absent, DefaultAspect is used — all methods are identity / allow-all,
// and the empty CursorContext is optimised away via [[no_unique_address]].
//
// Aspects are shared: one instance per _DB.  Per-cursor state lives in
// CursorContext, which the Aspect initialises via init_cursor_context().
//
// Join points:
//   Cursor-level — on_write, on_read, may_delete, init_big_meta
//   Merge-level  — may_merge_overwrite, may_merge_add, may_merge_delete

struct DefaultAspect {
  /// Extra bytes stored inline alongside _BigValue in big-value leaves.
  /// Set to a non-zero value to store per-leaf metadata (e.g. version
  /// vectors) that can be read without touching chunk storage.
  static constexpr size_t big_meta_size = 0;

  /// Per-cursor state.  The cursor holds one instance; the Aspect
  /// receives it by reference at every join point.
  struct CursorContext {};

  // --- Lifecycle ----------------------------------------------------------

  /// Called once when a cursor is created.  Use to initialise per-session
  /// state (scratch buffers, actor ids, keys, …).
  void init_cursor_context(CursorContext&) {}

  // --- Cursor join points -------------------------------------------------

  /// Called before writing a value.  May transform the value by writing
  /// into the CursorContext's scratch buffer and returning a Slice
  /// pointing into it.  Return the input unchanged for pass-through.
  Slice on_write(const Slice& key, const Slice& value, CursorContext&) {
    return value;
  }

  /// Called after reading a raw value from storage.
  /// |big_meta| is non-empty only when the leaf is big-valued and
  ///  big_meta_size > 0; it contains the inline metadata bytes.
  Slice on_read(const Slice& key, const Slice& data,
                const Slice& big_meta, CursorContext&) {
    return data;
  }

  /// Called before a cursor remove().  Return true to allow, false to
  /// reject.  The Aspect may also throw an exception to reject with a
  /// specific error.
  /// |value| is the current value at the cursor position.
  bool may_delete(const Slice& key, const Slice& value, CursorContext&) {
    return true;
  }

  /// Called when a big value is allocated.  Write metadata into |meta_ptr|
  /// (exactly big_meta_size bytes).
  void init_big_meta(const Slice& key, char* meta_ptr, CursorContext&) {}

  // --- Merge join points --------------------------------------------------
  // These are called during replication merge.  The CursorContext belongs
  // to the merge policy (not a user cursor).

  /// Called before overwriting an existing leaf during merge.
  bool may_merge_overwrite(const Slice& key, const Slice& dst, bool dst_is_big,
                           const Slice& src, bool src_is_big,
                           CursorContext&) {
    return true;
  }

  /// Called before adding a new leaf during merge.
  bool may_merge_add(const Slice& key, const Slice& value, bool is_big,
                     CursorContext&) {
    return true;
  }

  /// Called before deleting a key during deletion-trie merge.
  /// |meta| contains the metadata stored alongside the deletion timestamp.
  bool may_merge_delete(const Slice& key, const Slice& meta,
                        CursorContext&) {
    return true;
  }

  // --- Replication join points --------------------------------------------

  /// Called by background hash catchup when a transaction's hashes are ready.
  /// Applications can use this to trigger non-blocking replication sessions.
  /// |db| is the ReplicationDB, |txn_id| is the transaction that was hashed.
  template <typename DB>
  void on_hashes_ready(DB* db, tid_t txn_id) {}
};

}  // namespace leaves

#endif  // _LEAVES_ASPECT_HPP
