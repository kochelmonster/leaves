#include "storage.hpp"
#include "node.hpp"

namespace larch_leaves {

struct Trace {
  Trace(Storage& storage) : storage(storage), version(*storage.version) {
    current_key.reserve(1024);
  }

  bool valid() const { return rest_key.empty() && stack.back().valid(); }
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(const Slice& value);
  Slice get_value() const { return stack.back().get_value(); }
  void remove();


  void ifind(Transition transition);
  void sanitize() {
    while(version != *storage.version) {
      // restore trace after another cursor changed the trie.
      find(current_key);
    }
  }

  std::vector<Transition> stack;
  Storage& storage;
  Slice rest_key;
  std::string current_key;
  uint64_t version;
};
}
