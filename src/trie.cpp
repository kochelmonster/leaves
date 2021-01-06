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


offset_ptr* Trie::find(Transition& self, ISlice& key) {
  if (key.empty()) {
    self.cmp = 1;
    return NULL;
  }

  offset_ptr *result = ifind(self, key[0]);
  if (result)
    key.iadvance(1);

  return result;
}

offset_ptr* Trie::ifind(Transition& self, char key) {
  self.cmp = 0;
  char value = self.key = key;
  self.second_ptr = self.upper->find(bit::upper(value));
  if (self.second_ptr) {
    assert(self.second_ptr->resolve().node->type == kTrie);
    uint8_t lower = bit::lower(value);
    self.lower = self.second_ptr->resolve().trie;
    return self.lower->find(lower);
  }
  self.lower = NULL;
  return NULL;
}

TrieNavigation* Trie::next(Transition& self) {
  any_ptr next;
  if (self.cmp == 1) {
    return rfirst(self.upper);
  }

  if (self.lower)
    next = self.lower->next(bit::lower(self.key));
  else
    next = self.upper->next(bit::upper(self.key));

  return next.as_int ? rfirst(next) : NULL;
}

TrieNavigation* Trie::first(any_ptr node) {
  assert(node.node->type == kTrie);
  return rfirst(node.trie->first());
}

int Trie::advance(Transition& self, ISlice& key) {
  if (key.size() && key[0] == self.key) {
    self.cmp = 0;
    key.iadvance(1);
    return 1;
  }
  return -1;
}

void Trie::insert(Transition& self, ISlice& key, const Slice& value, TrieNavigation* next_leaf) {
  any_ptr leaf = LeafData::build(self.storage, key, value, next_leaf);

  if (self.cmp == 1) {
    // insert_trie_short: (key was empty at find -> insert value key before)
    leaf.leaf->next = *self.node_ptr;
    self.set(leaf);
    return;
  }

  // insert_trie_split:
  uint8_t upper = bit::upper(self.key);
  uint8_t lower = bit::lower(self.key);

  if (!self.lower) {
    any_ptr lower_ptr = TrieData::create(self.storage, leaf, lower);
    self.lower = lower_ptr.trie;
    self.set(self.upper->insert(self, lower_ptr, upper));
    return;
  }

  any_ptr result = self.lower->insert(self, leaf, lower);
  *self.second_ptr = result;
  self.lower = result.trie;
}


bool Trie::remove(Transition& self) {
  if (self.lower->remove(self, &self.lower, self.second_ptr,  bit::lower(self.key))) {
    self.upper->remove(self, &self.upper, self.node_ptr, bit::upper(self.key));
    return true;
  }

  if (popcount(self.upper->bits) == 1 && popcount(self.lower->bits) == 1) {
    any_ptr next = self.lower->children[0].resolve();
    char value = (ctz(self.upper->bits) << 4) | ctz(self.lower->bits);
    self.storage->free(self.lower);
    self.storage->free(self.upper);
    self.set(CompressedData::build(self.storage, next, Slice(&value, 1)));
    self.remove();  // combines compressed if possible
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
    any_ptr new_ptr = self.storage->allocate(size_of(count+1)).type(kTrie);
    copy_to(new_ptr.trie, count);
    self.storage->free(this);
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
    self.storage->free(this);
    *link = offset_ptr();
    return true;
  }

  size_t count = popcount(bits);
  for(size_t i = index; i < count; i++) {
    children[i] = children[i+1];
  }

  if (full(count)) {
    // shrink
    any_ptr new_ptr = self.storage->allocate(size_of(count)).type(kTrie);
    copy_to(new_ptr, count);
    *dest = new_ptr.trie;
    *link = new_ptr;
    self.storage->free(this);
  }
  return false;
}

any_ptr TrieData::create(Storage* storage, any_ptr next, int bit) {
  any_ptr result = storage->pools[0].allocate().type(kTrie);
  result.trie->bits = 1<<bit;
  result.trie->children[0] = next;
  return result;
}

any_ptr TrieData::build(Storage* storage, any_ptr next, char key) {
  next = create(storage, next, bit::lower(key));
  next = create(storage, next, bit::upper(key));
  return next;
}

} // namespace leaves
