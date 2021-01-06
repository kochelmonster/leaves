#include "storage.hpp"
#include "node.hpp"

namespace leaves {


struct Trace {
  typedef std::vector<Transition> stack_type;

  Trace(Storage& storage, TrieNavigation* root_=NULL)
      : storage(storage), root(root_), cursor(root_), version(*storage.version) {

    if (!root_)
      root = cursor = storage.master;

    current_key.reserve(1024);
    stack.reserve(1024);
  }

  bool valid() const { return cursor->type == kLeaf; }
  void find(const Slice& key);
  void first() { set_cursor(root->next_leaf.resolve()); }
  void last() { set_cursor(root->prev_leaf.resolve()); }
  void next() { set_cursor(cursor->next_leaf.resolve()); }
  void prev() { set_cursor(cursor->prev_leaf.resolve()); }

  void set_cursor(any_ptr ptr) {
    cursor = ptr.navigation;
    if (valid())
      current_key.assign(ptr.leaf->chars, ptr.leaf->size);
  }

  void set_value(const Slice& value);
  Slice get_value() const { return ((LeafData*)cursor)->get_value(); }
  void remove();

  void ifind();
  TrieNavigation* ifind_next();

  void sanitize() {
    while(version != *storage.version) {
      // restore trace after another cursor changed the trie.
      stack.clear();
      find(current_key);
    }
  }

  stack_type stack;
  Storage& storage;
  TrieNavigation *root;
  TrieNavigation* cursor;
  ISlice rest_key;
  string current_key;
  uint64_t version;
};
} // namespace leaves
