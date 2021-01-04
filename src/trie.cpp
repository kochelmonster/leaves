/* handling trie nodes
*/
#include "trie.hpp"

namespace leaves {

namespace bit {
  uint8_t upper(uint8_t value) {
    return value >> 4;
  }

  uint8_t lower(uint8_t value) {
    return (value & 0x0F);
  }
}


segment_ptr* Trie::find(Transition& self, ISlice& key, string& current_key) {
  if (key.empty()) {
    self.cmp = 1;
    return NULL;
  }

  segment_ptr *result = ifind(self, key[0]);
  current_key.push_back(self.key);
  if (result)
    key.iadvance(1);

  return result;
}

segment_ptr* Trie::ifind(Transition& self, char key) {
  self.cmp = 0;
  char value = self.key = key;
  self.second_ptr = self.upper->find(bit::upper(value));
  if (self.second_ptr) {
    assert(self.second_ptr->type == kTrie);
    uint8_t lower = bit::lower(value);
    self.lower = self.resolve(*self.second_ptr).trie;
    return self.lower->find(lower);
  }
  else
    self.lower = NULL;
  return NULL;
}

segment_ptr* Trie::next(Transition& self, string& current_key) {
  if (self.cmp == 1)
    return self.first(current_key);

  uint8_t upper(bit::upper(self.key)), lower;
  if (self.lower) {
    assert(self.second_ptr->type == kTrie);
    lower = bit::lower(self.key);
    segment_ptr* next = self.lower->next(lower);
    if (next) {
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }
  }

  self.second_ptr = self.upper->next(upper);
  if (self.second_ptr) {
    self.lower = self.resolve(*self.second_ptr).trie;
    segment_ptr* next = self.lower->first(lower);
    current_key.back() = self.key = (upper << 4) | lower;
    return next;
  }

  current_key.pop_back();
  return NULL;
}

segment_ptr* Trie::first(Transition& self, string& current_key) {
  uint8_t upper, lower;
  self.cmp = 0;
  self.second_ptr = self.upper->first(upper);
  self.lower = self.resolve(*self.second_ptr).trie;
  segment_ptr *next = self.lower->first(lower);
  current_key.push_back(self.key = (upper << 4) | lower);
  return next;
}

segment_ptr* Trie::prev(Transition& self, string& current_key) {
  if (self.cmp == 1)
    return NULL;

  uint8_t upper(bit::upper(self.key)), lower;
  if (self.lower) {
    assert(self.second_ptr->type == kTrie);
    lower = bit::lower(self.key);
    segment_ptr* next = self.lower->prev(lower);
    if (next) {
      current_key.back() = self.key = (upper << 4) | lower;
      return next;
    }
  }

  self.second_ptr = self.upper->prev(upper);
  if (self.second_ptr) {
    self.lower = self.resolve(*self.second_ptr).trie;
    segment_ptr* next = self.lower->last(lower);
    current_key.back() = self.key = (upper << 4) | lower;
    return next;
  }

  current_key.pop_back();
  return NULL;
}

segment_ptr* Trie::last(Transition& self, string& current_key) {
  uint8_t upper, lower;
  self.cmp = 0;
  self.second_ptr = self.upper->last(upper);
  self.lower = self.resolve(*self.second_ptr).trie;
  segment_ptr *next = self.lower->last(lower);
  current_key.push_back(self.key = (upper << 4) | lower);
  return next;
}

int Trie::advance(Transition& self, ISlice& key) {
  if (key.size() && key[0] == self.key) {
    self.cmp = 0;
    key.iadvance(1);
    return 1;
  }
  return -1;
}

void Trie::insert(Transition& self, ISlice& key, const Slice& value, string& current_key) {
  if (self.cmp == 1) {
    // key was empty at find -> insert value key before
    self.set(ValueData::build(self.storage, *self.node_ptr, value));
    return;
  }

  current_key.pop_back();
  uint8_t upper = bit::upper(self.key);
  uint8_t lower = bit::lower(self.key);

  resolved_ptr next(ValueData::build(self.storage, segment_ptr(), value));
  if (key.size() > 1) {
    Slice restkey(key.advance(1));
    next = CompressedData::build(self.storage, next, restkey);
  }

  if (!self.lower) {
    resolved_ptr lower_ptr = TrieData::create(self.storage, next.me, lower);
    self.set(self.upper->insert(self, self.node_ptr, lower_ptr.me, upper));
    return;
  }

  resolved_ptr result = self.lower->insert(self, self.second_ptr, next.me, lower);
  *self.second_ptr = result.me;
  self.lower = result.trie;
}


bool Trie::remove(Transition& self, bool last) {
  if (self.lower->remove(self, self.second_ptr, &self.lower, bit::lower(self.key))) {
    self.upper->remove(self, self.node_ptr, &self.upper, bit::upper(self.key));
  }

  if (popcount(self.upper->bits) == 1 && popcount(self.lower->bits) == 1) {
    resolved_ptr next = self.resolve(self.lower->children[0]);
    char value = (ctz(self.upper->bits) << 4) | ctz(self.lower->bits);
    self.storage->pools[0].free(resolved_ptr(*self.second_ptr, self.lower));
    self.storage->pools[0].free(resolved_ptr(*self.node_ptr, self.upper));
    self.set(CompressedData::build(self.storage, next, Slice(&value, 1)));
    self.remove(last);  // combines compressed if possible
  }
  return true;
}



/* Trie Data
---------------------------------------------------------------------------------
*/


void TrieData::add(int bit, segment_ptr next) {
  int moved_bit = 1 << bit;
  assert(!(bits & moved_bit));
  bits |= moved_bit;
  int index = index_of_moved(moved_bit);
  for(int i = popcount(bits)-1; i > index; i--) {
    children[i] = children[i-1];
  }
  children[index] = next;
}

resolved_ptr TrieData::insert(Transition& self, segment_ptr* to_me, segment_ptr next, int bit) {
  size_t count = popcount(bits);
  if (full(count)) {
    // node must grow
    size_t size = size_of(count);
    resolved_ptr new_ptr = self.storage->allocate(size+sizeof(segment_ptr)).type(kTrie);
    memcpy((void*)new_ptr.trie, this, size);
    self.storage->free(resolved_ptr(*to_me, this), size);
    new_ptr.trie->add(bit, next);
    return new_ptr;
  }

  add(bit, next);
  return resolved_ptr(*to_me, this);
}

bool TrieData::remove(Transition& self, segment_ptr* to_me, TrieData** dest, int bit) {
  assert(bits & (1<<bit));
  int index = index_of(bit);

  if (children[index]) {
    // the node is still active => remove of intermediate value
    return false;
  }

  bits &= ~(1<<bit);

  if (!bits) {
    // has been shrunken to pool 0 for shure
    self.storage->pools[0].free(resolved_ptr(*to_me, this));
    *to_me = segment_ptr();
    return true;
  }

  size_t count = popcount(bits);
  for(size_t i = index; i < count; i++) {
    children[i] = children[i+1];
  }

  if (full(count)) {
    // shrink
    size_t size = size_of(count);
    resolved_ptr new_ptr = self.storage->allocate(size);
    new_ptr.me.type = kTrie;
    memcpy((void*)new_ptr.trie, this, size);
    *dest = new_ptr.trie;
    self.storage->free(resolved_ptr(*to_me, this), size+sizeof(segment_ptr));
    *to_me = new_ptr;
  }
  return false;
}

resolved_ptr TrieData::create(Storage* storage, segment_ptr next, int bit) {
  resolved_ptr result = storage->pools[0].allocate();
  result.trie->bits = 1<<bit;
  result.trie->children[0] = next;
  result.me.type = kTrie;
  return result;
}

resolved_ptr TrieData::build(Storage* storage, segment_ptr next, char key) {
  resolved_ptr rnext = create(storage, next, bit::lower(key));
  rnext = create(storage, rnext.me, bit::upper(key));
  return rnext;
}

} // namespace leaves
