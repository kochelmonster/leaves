/* handling trie nodes
*/
#include "trie.hpp"
#include "trace.hpp"

namespace leaves {

namespace bit {
  uint8_t upper(uint8_t value) {
    return value >> 4;
  }

  uint8_t lower(uint8_t value) {
    return (value & 0x0F);
  }
}


offset_ptr* Trie::find(Transition& self, ISlice& key, string& current_key) {
  if (key.empty()) {
    self.cmp = 1;
    return NULL;
  }

  offset_ptr *result = ifind(self, key[0]);
  current_key.push_back(self.key);
  if (result) {
    self.cmp = 0;
    key.iadvance(1);
  }
  return result;
}

offset_ptr* Trie::ifind(Transition& self, char key) {
  char value = self.key = key;
  self.cmp = -1;
  self.second_ptr = self.upper->find(bit::upper(value));
  if (self.second_ptr) {
    assert(self.second_ptr->resolve().node->type == kTrie);
    uint8_t lower = bit::lower(value);
    self.lower = self.second_ptr->resolve().trie;
    return self.lower->find(lower);
  }
  else
    self.lower = NULL;
  return NULL;
}

offset_ptr* Trie::next(Transition& self, string& current_key) {
  if (self.cmp == 1)
    return self.first(current_key);

  uint8_t upper(bit::upper(self.key)), lower;
  if (self.lower) {
    assert(self.second_ptr->resolve().node->type == kTrie);
    lower = bit::lower(self.key);
    offset_ptr* next = self.lower->next(lower);
    if (next) {
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }
  }

  self.second_ptr = self.upper->next(upper);
  if (self.second_ptr) {
    self.lower = self.second_ptr->resolve().trie;
    offset_ptr* next = self.lower->first(lower);
    current_key.back() = self.key = (upper << 4) | lower;
    return next;
  }

  current_key.pop_back();
  return NULL;
}

offset_ptr* Trie::first(Transition& self, string& current_key) {
  uint8_t upper, lower;
  self.cmp = 0;
  self.second_ptr = self.upper->first(upper);
  self.lower = self.second_ptr->resolve().trie;
  offset_ptr *next = self.lower->first(lower);
  current_key.push_back(self.key = (upper << 4) | lower);
  return next;
}

offset_ptr* Trie::prev(Transition& self, string& current_key) {
  if (self.cmp == 1)
    return NULL;

  uint8_t upper(bit::upper(self.key)), lower;
  if (self.lower) {
    assert(self.second_ptr->resolve().node->type == kTrie);
    lower = bit::lower(self.key);
    offset_ptr* next = self.lower->prev(lower);
    if (next) {
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }
  }

  self.second_ptr = self.upper->prev(upper);
  if (self.second_ptr) {
    self.lower = self.second_ptr->resolve().trie;
    offset_ptr* next = self.lower->last(lower);
    current_key.back() = self.key = (upper << 4) | lower;
    return next;
  }

  current_key.pop_back();
  return NULL;
}

offset_ptr* Trie::last(Transition& self, string& current_key) {
  uint8_t upper, lower;
  self.cmp = 0;
  self.second_ptr = self.upper->last(upper);
  self.lower = self.second_ptr->resolve().trie;
  offset_ptr *next = self.lower->last(lower);
  current_key.push_back(self.key = (upper << 4) | lower);
  return next;
}

int Trie::advance(Transition& self, ISlice& key) {
  if (key.size() && key[0] == self.key && self.cmp == 0) {
    key.iadvance(1);
    return 1;
  }
  return -1;
}

void Trie::insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
  if (self.cmp == 1) {
    // key was empty at find -> insert value key before
    self.set(ValueData::build(self.trace, *self.node_ptr, value));
    return;
  }

  current_key.pop_back();
  uint8_t upper = bit::upper(self.key);
  uint8_t lower = bit::lower(self.key);

  any_ptr next(ValueData::build(self.trace, offset_ptr(), value));
  if (key.size() > 1) {
    Slice restkey(key.advance(1));
    next = CompressedData::build(self.trace, next, restkey);
  }

  if (!self.lower) {
    any_ptr lower_ptr = TrieData::create(self.trace, next, lower);
    self.lower = lower_ptr.trie;
    self.set(self.upper->insert(self, lower_ptr, upper));
    return;
  }

  any_ptr result = self.lower->insert(self, next, lower);
  *self.second_ptr = result;
  self.lower = result.trie;
}


bool Trie::remove(Transition& self, bool last) {
  if (self.lower->remove(self, &self.lower, self.second_ptr,  bit::lower(self.key))) {
    self.upper->remove(self, &self.upper, self.node_ptr, bit::upper(self.key));
    return true;
  }

  if (popcount(self.upper->bits) == 1 && popcount(self.lower->bits) == 1) {
    any_ptr next = self.lower->children[0].resolve();
    char value = (ctz(self.upper->bits) << 4) | ctz(self.lower->bits);
    self.trace->free(self.lower);
    self.trace->free(self.upper);
    self.set(CompressedData::build(self.trace, next, Slice(&value, 1)));
    self.remove(last);  // combines compressed if possible
  }
  return true;
}


/* Trie Data
---------------------------------------------------------------------------------
*/

void TrieData::add(int bit, any_ptr next) {
  int moved_bit = 1 << bit;
  assert(!(bits & moved_bit));
  bits |= moved_bit;
  int index = index_of_moved(moved_bit);
  for(int i = popcount(bits)-1; i > index; i--) {
    children[i] = children[i-1];
  }
  children[index] = next;
}

void TrieData::copy_to(any_ptr dest, size_t count) {
  dest.trie->bits = bits;
  for(size_t i = 0; i < count; i++) {
    dest.trie->children[i] = children[i];
  }
}

any_ptr TrieData::insert(Transition& self, any_ptr next, int bit) {
  size_t count = popcount(bits);
  if (full(count)) {
    // node must grow
    any_ptr new_ptr = self.trace->allocate(size_of(count+1)).type(kTrie);
    copy_to(new_ptr.trie, count);
    self.trace->free(this);
    new_ptr.trie->add(bit, next);
    return new_ptr;
  }

  add(bit, next);
  return this;
}

bool TrieData::remove(Transition& self, TrieData** dest, offset_ptr *link, int bit) {
  assert(bits & (1<<bit));
  int index = index_of(bit);

  if (children[index]) {
    // the node is still active => remove of intermediate value
    return false;
  }

  bits &= ~(1<<bit);

  if (!bits) {
    // has been shrunken to pool 0 for shure
    self.trace->free(this);
    *link = offset_ptr();
    return true;
  }

  size_t count = popcount(bits);
  for(size_t i = index; i < count; i++) {
    children[i] = children[i+1];
  }

  if (full(count)) {
    // shrink
    any_ptr new_ptr = self.trace->allocate(size_of(count)).type(kTrie);
    copy_to(new_ptr, count);
    *dest = new_ptr.trie;
    *link = new_ptr;
    self.trace->free(this);
  }
  return false;
}

any_ptr TrieData::create(Trace* trace, any_ptr next, int bit) {
  any_ptr result = trace->storage.pools[0].allocate().type(kTrie);
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
