#include "storage.hpp"
#include "node.hpp"

namespace larch_leaves {


struct Trace {
  Trace(Storage* storage) : storage(storage) {
    key.reserve(1024);
  }

  void find(Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void insert(Slice& key, Slice& value);
  void remove();

  std::vector<Transition> stack;
  Storage* storage;
  uint64_t version;
};


void Trace::find(Slice& key) {
  this->key = key;
  Slice _key(key);

  Transition transition;
  segment_ptr *next = &storage->start;

  stack.clear();
  while(next) {
    transition = Transition(next, storage);
    stack.push_back(transition);
    next = transition.find(_key);
  }

  next = transition.first();
  while(next) {
    transition = Transition(next, storage);
    stack.push_back(transition);
    next = transition.first();
  }
  // stack.back points to a leave
}

void insert(Slice key, Slice& value) {
  size_t i = 0;
  Transition transition;
  segment_ptr *next = &storage->start;

  stack.clear();
  while(next) {
    transition = Transition(next, storage);
    stack.push_back(transition);
    next = transition.find(key);
  }
  next = transition.insert(key, value);
  stack.pop_back(transition);
  transition = Transition(next, storage);
  stack.push_back(transition);

  next = transition.find(key);
  while(next) {
    transition = Transition(next, storage);
    stack.push_back(transition);
    next = transition.find(key);
  }
}
}
