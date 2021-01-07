#include "storage.hpp"
#include "node.hpp"

namespace leaves {

typedef offset_ptr* (*move_func_t)(Transition& transition, string& current_key);


struct Trace {
  typedef std::vector<Transition> stack_type;

  Trace(Storage& storage, offset_ptr* root_=NULL) : storage(storage), version(*storage.version) {
    current_key.reserve(1024);
    stack.reserve(1024);
    if (!root)
      root = storage.root;
  }

  bool valid() const { return rest_key.empty() && stack.size() && stack.back().valid(); }
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(const Slice& value);
  Slice get_value() const { return stack.back().get_value(); }
  void remove();

  any_ptr allocate(size_t size) { return storage.allocate(size); }
  void free(any_ptr ptr) { return storage.free(ptr); }

  void ifind();
  void iremove();
  void imove_end(move_func_t move);
  void imove(move_func_t move, move_func_t move_end);

  void sanitize() {
    while(version != *storage.version) {
      // restore trace after another cursor changed the trie.
      find(current_key);
    }
  }

  stack_type stack;
  Storage& storage;
  offset_ptr* root;
  ISlice rest_key;
  string current_key;
  uint64_t version;
};

template <typename type1, typename type2>
void Transition::set_value(type1& dest, type2 src) {
  trace->storage.set_value(dest, src);
}

inline void Transition::memcpy(void* dest, const void* src, size_t size) {
  trace->storage.memcpy(dest, src, size);
}

inline void Transition::memmove(void* dest, const void* src, size_t size) {
  trace->storage.memmove(dest, src, size);
}

inline void Transition::memset(void* dest, char val, size_t size) {
  trace->storage.memset(dest, val, size);
}


} // namespace leaves
