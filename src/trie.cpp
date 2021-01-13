/* handling trie nodes
*/
#include "trie.hpp"
#include "table.hpp"
#include "trace.hpp"

namespace leaves {

offset_ptr* Trie::find(Transition& self, ISlice& key, KeyString& current_key) {
  if (key.empty()) {
    self.cmp = -1;
    return NULL;
  }

  char ckey = key[0];
  offset_ptr *result = ifind(self, ckey);
  if (result) {
    current_key.push_back(ckey);
    key.iadvance(1);
  }
  return result;
}

offset_ptr* Trie::ifind(Transition& self, char key) {
  self.key = bit::upper(key);
  if (self.lower.set(self.trie->find(self))) {
    self.lower.key = bit::lower(key);
    return self.lower.trie->find(self.lower);
  }
  return NULL;
}

offset_ptr* Trie::next(Transition& self, KeyString& current_key) {
  offset_ptr* result = NULL;

  if (self.cmp == -1) {
    // empty key
    return self.first(current_key);
  }

  if (self.lower.trie) {
    if (!self.lower.cmp)
      current_key.pop_back();
    result = self.lower.trie->next(self.lower);
  }

  if (!result) {
    if (self.lower.set(self.trie->next(self)))
      result = self.lower.trie->first(self.lower);
  }

  if (result)
    current_key.push_back(to_char(self));

  return result;
}

offset_ptr* Trie::first(Transition& self, KeyString& current_key) {
  self.lower.set(self.trie->first(self));
  offset_ptr *result = self.lower.trie->first(self.lower);
  current_key.push_back(to_char(self));
  return result;
}

offset_ptr* Trie::prev(Transition& self, KeyString& current_key) {
  offset_ptr *result = NULL;

  if (self.cmp == -1)
    return NULL;

  if (self.lower.trie) {
    if (!self.lower.cmp)
      current_key.pop_back();
    result = self.lower.trie->prev(self.lower);
  }

  if (!result) {
    if (self.lower.set(self.trie->prev(self)))
      result = self.lower.trie->last(self.lower);
  }

  if (result)
    current_key.push_back(to_char(self));

  return result;
}


offset_ptr* Trie::last(Transition& self, KeyString& current_key) {
  self.lower.set(self.trie->last(self));
  offset_ptr *result = self.lower.trie->last(self.lower);
  current_key.push_back(to_char(self));
  return result;
}

int Trie::advance(Transition& self, const Slice& key) {
  return key.size() && self.lower.cmp == 0 && key[0] == to_char(self) ? 1 : -1;
}

void Trie::insert(Transition& self, const Slice& key, any_ptr next) {
  if (self.cmp == -1) {
    // key was empty at find -> insert value key before
    assert(next.node->type == kValue);
    next.value->next = *self.node_ptr;
    self.set(next);
    return;
  }

  if (key.size() > 1) {
    Slice restkey(key.advance(1));
#if PURE_TRIE
    next = CompressedData::build(self.trace, next, restkey);
#else
    next = TableData::build(self.trace, next, restkey);
#endif
  }

  if (self.lower.trie)
    self.lower.trie->insert(self.lower, self.trace, next);
  else
    self.trie->insert(self, self.trace, TrieData::create(self.trace, next, self.lower.key));
}

bool Trie::remove(Transition& self) {
  if (self.lower.trie->remove(self.lower, self.trace)) {
    self.trie->remove(self, self.trace);
    return true;
  }

  if (one_branch(self)) {
    any_ptr next = self.lower.trie->children[0].resolve();
    char key = to_char(self);
    self.trace->free(self.trie);
    self.trace->free(self.lower.trie);
    self.set(CompressedData::build(self.trace, next, Slice(&key, 1)));
    self.remove();  // combines compressed if possible
  }
  return true;
}

void Trie::report(offset_ptr* node, Stats& stats) {
  TransitionData upper;
  TransitionData lower;
  uint16_t count = 0;
  upper.set(node);
  lower.set(upper.trie->first(upper));
  do {
    offset_ptr *next = lower.trie->first(lower);
    do {
      count++;
      Transition::handlers[next->resolve().node->type]->report(next, stats);
    } while((next = lower.trie->next(lower)));
  }
  while(lower.set(upper.trie->next(upper)));
  stats.tries_nodes[count]++;
}


Trie trie_handler;

/* Trie Data
---------------------------------------------------------------------------------
*/

void TrieData::add(int bit, any_ptr next, size_t count) {
  int moved_bit = 1 << bit;
  assert(!(bits & moved_bit));

  int index = index_of_moved(moved_bit);
  for(int i = count; i > index; i--)
    children[i] = children[i-1];

  children[index] = next;
  bits |= moved_bit;
}

void TrieData::copy_to(any_ptr dest, size_t count) {
  dest.trie->bits = bits;
  for(size_t i = 0; i < count; i++) {
    dest.trie->children[i] = children[i];
  }
}

void TrieData::insert(TransitionData& self, Trace* trace, any_ptr next) {
  size_t count = popcount(bits);
  if (full(count)) {
    // node must grow
    any_ptr new_ptr = trace->allocate(size_of(count+1));
    new_ptr.trie->type = kTrie;
    copy_to(new_ptr.trie, count);
    new_ptr.trie->add(self.key, next, count+1);
    self.set(new_ptr);
    trace->free(this);
    return;
  }
  add(self.key, next, count);
}

bool TrieData::remove(TransitionData& self, Trace* trace) {
  assert(bits & (1<<self.key));
  int index = index_of(self.key);

  if (children[index]) {
    // the node is still active => remove of intermediate value
    return false;
  }

  bits &= ~(1<<self.key);
  if (!bits) {
    // has been shrunken to pool 0 for shure
    trace->free(this);
    *self.node_ptr = offset_ptr();
    return true;
  }

  size_t count = popcount(bits);
  for(size_t i = index; i < count; i++)
    children[i] = children[i+1];

  if (full(count)) {
    // shrink
    any_ptr new_ptr = trace->allocate(size_of(count));
    new_ptr.trie->type = kTrie;
    copy_to(new_ptr, count);
    self.set(new_ptr.trie);
    trace->free(this);
  }
  return false;
}

any_ptr TrieData::create(Trace* trace, any_ptr next, int bit) {
  any_ptr result = trace->storage.allocate(sizeof(TrieData));
  result.trie->type = kTrie;
  result.trie->bits = 1<<bit;
  result.trie->children[0] = next;
  return result;
}

any_ptr TrieData::build(Trace* trace, any_ptr next, char key) {
  next = create(trace, next, bit::lower(key));
  next = create(trace, next, bit::upper(key));
  return next;
}

} // namespace leaves
