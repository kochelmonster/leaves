#ifndef _LEAVES_JS_ASPECT_HPP
#define _LEAVES_JS_ASPECT_HPP

/**
 * JSAspect: Aspect implementation that forwards all join points
 * to JavaScript callbacks via Emscripten bindings.
 *
 * Used with _BrowserStore<JSAspect> to allow JS users to intercept
 * cursor operations, transaction lifecycle, and merge decisions.
 *
 * All callbacks are optional -- if a JS method is not provided,
 * the default (allow-all / pass-through) behavior applies.
 */

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include <string>

#include "../include/leaves/intern/db/_aspect.hpp"

namespace leaves {

struct JSAspect : public DefaultAspect {
  static constexpr size_t big_meta_size = 0;

  struct CursorContext {
    emscripten::val js_context{emscripten::val::undefined()};
    std::string write_buf;  // scratch buffer for value transformation
  };

  /// JS callbacks object.  Set via set_callbacks() on the DB.
  emscripten::val _callbacks{emscripten::val::undefined()};

  void set_callbacks(emscripten::val cb) { _callbacks = cb; }

  // Shorthand: check if a named method exists on the callbacks object
  bool _has(const char* name) const {
    return _callbacks.as<bool>() && _callbacks[name].as<bool>();
  }

  // -----------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------

  void init_cursor_context(CursorContext& ctx) {
    if (_has("initCursorContext")) {
      ctx.js_context = _callbacks.call<emscripten::val>("initCursorContext");
    }
  }

  // -----------------------------------------------------------------
  // Cursor read/write
  // -----------------------------------------------------------------

  Slice on_write(const Slice& key, const Slice& value, CursorContext& ctx) {
    if (_has("onWrite")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_val = emscripten::val(emscripten::typed_memory_view(
          value.size(), reinterpret_cast<const uint8_t*>(value.data())));
      auto js_ctx = ctx.js_context;
      auto result = _callbacks.call<emscripten::val>("onWrite", js_key, js_val, js_ctx);
      if (result.as<bool>()) {
        std::string r = result.as<std::string>();
        ctx.write_buf = std::move(r);
        return Slice(ctx.write_buf);
      }
    }
    return value;
  }

  Slice on_read(const Slice& key, const Slice& data,
                const Slice& big_meta, CursorContext& ctx) {
    if (_has("onRead")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_data = emscripten::val(emscripten::typed_memory_view(
          data.size(), reinterpret_cast<const uint8_t*>(data.data())));
      auto js_meta = emscripten::val(emscripten::typed_memory_view(
          big_meta.size(), reinterpret_cast<const uint8_t*>(big_meta.data())));
      auto js_ctx = ctx.js_context;
      auto result = _callbacks.call<emscripten::val>("onRead", js_key, js_data, js_meta, js_ctx);
      if (result.as<bool>()) {
        std::string r = result.as<std::string>();
        ctx.write_buf = std::move(r);
        return Slice(ctx.write_buf);
      }
    }
    return data;
  }

  bool may_delete(const Slice& key, const Slice& value, CursorContext& ctx) {
    if (_has("mayDelete")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_val = emscripten::val(emscripten::typed_memory_view(
          value.size(), reinterpret_cast<const uint8_t*>(value.data())));
      auto js_ctx = ctx.js_context;
      return _callbacks.call<bool>("mayDelete", js_key, js_val, js_ctx);
    }
    return true;
  }

  void init_big_meta(const Slice& key, char* meta_ptr, CursorContext& ctx) {
    if (_has("initBigMeta")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_ctx = ctx.js_context;
      auto result = _callbacks.call<emscripten::val>("initBigMeta", js_key, js_ctx);
      if (result.as<bool>()) {
        std::string r = result.as<std::string>();
        size_t n = std::min(r.size(), size_t(8));
        std::memcpy(meta_ptr, r.data(), n);
      }
    }
  }

  // -----------------------------------------------------------------
  // Cursor-level transaction join points
  // -----------------------------------------------------------------

  template <typename DB, typename Ctx>
  bool before_start_transaction(DB&, TransactionOrigin origin, Ctx& ctx) {
    if (_has("beforeStartTransaction")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      return _callbacks.call<bool>("beforeStartTransaction", js_origin, _js_ctx(ctx));
    }
    return true;
  }

  template <typename DB, typename Ctx>
  void on_start_transaction(DB&, tid_t tid, TransactionOrigin origin, Ctx& ctx) {
    if (_has("onStartTransaction")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      _callbacks.call<void>("onStartTransaction", emscripten::val(uint64_t(tid)),
                            js_origin, _js_ctx(ctx));
    }
  }

  template <typename DB, typename Ctx>
  bool before_rollback(DB&, tid_t tid, TransactionOrigin origin, Ctx& ctx) {
    if (_has("beforeRollback")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      return _callbacks.call<bool>("beforeRollback", emscripten::val(uint64_t(tid)),
                                    js_origin, _js_ctx(ctx));
    }
    return true;
  }

  template <typename DB, typename Ctx>
  void on_rollback(DB&, tid_t tid, TransactionOrigin origin, Ctx& ctx) {
    if (_has("onRollback")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      _callbacks.call<void>("onRollback", emscripten::val(uint64_t(tid)),
                            js_origin, _js_ctx(ctx));
    }
  }

  template <typename DB, typename Ctx>
  bool before_commit(DB&, TransactionOrigin origin, Ctx& ctx) {
    if (_has("beforeCommit")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      return _callbacks.call<bool>("beforeCommit", js_origin, _js_ctx(ctx));
    }
    return true;
  }

  template <typename DB, typename Ctx>
  void on_commit(DB&, TransactionOrigin origin, Ctx& ctx) {
    if (_has("onCommit")) {
      auto js_origin = emscripten::val(_origin_str(origin));
      _callbacks.call<void>("onCommit", js_origin, _js_ctx(ctx));
    }
  }

  // -----------------------------------------------------------------
  // Cursor navigation join points
  // -----------------------------------------------------------------

  template <typename Ctx>
  bool before_find(const Slice& key, Ctx& ctx) {
    if (_has("beforeFind")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      return _callbacks.call<bool>("beforeFind", js_key, _js_ctx(ctx));
    }
    return true;
  }

  template <typename Ctx>
  void on_find(const Slice& key, bool found, Ctx& ctx) {
    if (_has("onFind")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      _callbacks.call<void>("onFind", js_key, found, _js_ctx(ctx));
    }
  }

  template <typename Ctx>
  void on_next(bool has_next, Ctx& ctx) {
    if (_has("onNext")) {
      _callbacks.call<void>("onNext", has_next, _js_ctx(ctx));
    }
  }

  template <typename Ctx>
  void on_prev(bool has_prev, Ctx& ctx) {
    if (_has("onPrev")) {
      _callbacks.call<void>("onPrev", has_prev, _js_ctx(ctx));
    }
  }

  // -----------------------------------------------------------------
  // Maintenance join points (DB-level, no CursorContext)
  // -----------------------------------------------------------------

  template <typename DB>
  void on_sanitize(DB&) {
    if (_has("onSanitize")) {
      _callbacks.call<void>("onSanitize");
    }
  }

  template <typename DB>
  bool before_defrag(DB&) {
    if (_has("beforeDefrag")) {
      return _callbacks.call<bool>("beforeDefrag");
    }
    return true;
  }

  template <typename DB>
  void on_defrag(DB&) {
    if (_has("onDefrag")) {
      _callbacks.call<void>("onDefrag");
    }
  }

  template <typename DB>
  bool before_reset(DB&) {
    if (_has("beforeReset")) {
      return _callbacks.call<bool>("beforeReset");
    }
    return true;
  }

  template <typename DB>
  void on_reset(DB&) {
    if (_has("onReset")) {
      _callbacks.call<void>("onReset");
    }
  }

  // -----------------------------------------------------------------
  // Merge join points (used during replication)
  // -----------------------------------------------------------------

  bool may_merge_overwrite(const Slice& key, const Slice& dst, bool dst_is_big,
                           const Slice& src, bool src_is_big,
                           CursorContext& ctx) {
    if (_has("mayMergeOverwrite")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_dst = emscripten::val(emscripten::typed_memory_view(
          dst.size(), reinterpret_cast<const uint8_t*>(dst.data())));
      auto js_src = emscripten::val(emscripten::typed_memory_view(
          src.size(), reinterpret_cast<const uint8_t*>(src.data())));
      return _callbacks.call<bool>("mayMergeOverwrite", js_key, js_dst, dst_is_big,
                                    js_src, src_is_big, _js_ctx(ctx));
    }
    return true;
  }

  bool may_merge_add(const Slice& key, const Slice& value, bool is_big,
                     CursorContext& ctx) {
    if (_has("mayMergeAdd")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_val = emscripten::val(emscripten::typed_memory_view(
          value.size(), reinterpret_cast<const uint8_t*>(value.data())));
      return _callbacks.call<bool>("mayMergeAdd", js_key, js_val, is_big, _js_ctx(ctx));
    }
    return true;
  }

  bool may_merge_delete(const Slice& key, const Slice& meta,
                        CursorContext& ctx) {
    if (_has("mayMergeDelete")) {
      auto js_key = emscripten::val(emscripten::typed_memory_view(
          key.size(), reinterpret_cast<const uint8_t*>(key.data())));
      auto js_meta = emscripten::val(emscripten::typed_memory_view(
          meta.size(), reinterpret_cast<const uint8_t*>(meta.data())));
      return _callbacks.call<bool>("mayMergeDelete", js_key, js_meta, _js_ctx(ctx));
    }
    return true;
  }

 private:
  static const char* _origin_str(TransactionOrigin o) {
    switch (o) {
      case TransactionOrigin::user: return "user";
      case TransactionOrigin::merge: return "merge";
      case TransactionOrigin::defrag: return "defrag";
    }
    return "unknown";
  }

  // Extract the JS context from either CursorContext or another context type.
  // For non-CursorContext types (used by DB-level hooks), we return undefined.
  static emscripten::val _js_ctx(CursorContext& ctx) {
    return ctx.js_context;
  }

  template <typename Ctx>
  static emscripten::val _js_ctx(Ctx&) {
    return emscripten::val::undefined();
  }
};

}  // namespace leaves

#endif  // __EMSCRIPTEN__

#endif  // _LEAVES_JS_ASPECT_HPP