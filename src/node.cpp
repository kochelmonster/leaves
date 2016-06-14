/*
  Handlers for all trie nodes
*/

#include "bittrie.hpp"
#include <string.h>

namespace larch_leaves {

struct LeafBase : public NodeHandler {
  size_t len(const Node *rnode) { return 0; }

  size_t count(const Node *node) { return 0; }

  void next(Trace &trace) { trace.parent_next(); }

  void prev(Trace &trace) { trace.parent_prev(); }

  void first(Trace &trace) {}

  void last(Trace &trace) {}
};

struct TrieBase : public NodeHandler {
  size_t len(const Node *node) { return 1; }

  void data(Node *node, const void *data) {}

  Slice data(const Node *node) { return Slice(); }

  virtual void add_node(NodeRef &rnode, trieindex_t index, const TempNode &node,
                        Trace &trace) = 0;

  void add(const TempNode &leaf, Trace &trace) {
    Slice key(trace.current_key());
    NodeRef &rnode(trace.current());

    switch (key.size()) {
    case 0:
      TESTPOINT(TrieBaseAdd0);
      leaf.add_to(trace);
      trace.parent().node->children_[0] = trace.connect_ptr();
      break;

    case 1:
      TESTPOINT(TrieBaseAdd1);
      add_node(rnode, key[0], leaf, trace);
      break;

    default: {
      TESTPOINT(TrieBaseAdd2);
      Slice data(key.advance(1));
      TempCompressed compressed(Slice(
          data.data(), std::min(data.size(), (size_t)MAX_COMPRESSED_LEN)));
      add_node(rnode, key[0], compressed, trace);
      add_compressed(data.advance(compressed.node()->len()), trace);
      trace.add(leaf);
    }
    }
  }
};

struct CompressedHandler : public NodeHandler {
  inline char *raw_data(const Node *node) {
    return ((char *)node) + sizeof(Node::header_t) + sizeof(Page::ptr);
  }

  inline char *raw_data(const NodeRef &rnode) { return raw_data(rnode.node); }

  inline size_t raw_size(const Node *node) {
    return node->size - sizeof(Node::header_t) - sizeof(Page::ptr);
  }

  inline size_t raw_size(const NodeRef &rnode) { return raw_size(rnode.node); }

  //   < 0 if key <  data
  // returns
  //  == 0 if key == data
  //   > 0 if key > data
  int keycmp(Trace &trace) {
    Slice key(trace.current_key());
    NodeRef &rnode(trace.current());
    char *data = raw_data(rnode);
    size_t len = raw_size(rnode);

    int cmp = memcmp(key.data(), data, std::min(len, key.size()));
    if (cmp != 0)
      return cmp;

    return key.size() >= len ? 0 : -1;
  }

  void keyappend(Trace &trace) {
    NodeRef &rnode(trace.current());
    trace.cut_key();
    char *data = raw_data(rnode);
    size_t len = raw_size(rnode);
    trace.key.append(data, len);
  }

  size_t count(const Node *node) { return 1; }

  size_t len(const Node *node) { return raw_size(node); }

  void data(Node *node, const void *data) {
    memcpy(raw_data(node), data, raw_size(node));
  }

  Slice data(const Node *node) { return Slice(raw_data(node), raw_size(node)); }

  void find(Trace &trace) {
    NodeRef &rnode(trace.current());
    if (keycmp(trace) == 0 && rnode.node->children_[0])
      rnode.child_find(rnode.node->children_[0], trace);
  }

  void first(Trace &trace) {
    NodeRef &rnode(trace.current());
    keyappend(trace);
    rnode.child_first(rnode.node->children_[0], trace);
  }

  void last(Trace &trace) {
    NodeRef &rnode(trace.current());
    keyappend(trace);
    rnode.child_last(rnode.node->children_[0], trace);
  }

  void next(Trace &trace) {
    if (keycmp(trace) < 0) {
      NodeRef &rnode(trace.current());
      keyappend(trace);
      rnode.child_first(rnode.node->children_[0], trace);
    } else {
      trace.parent_next();
    }
  }

  void prev(Trace &trace) {
    if (keycmp(trace) > 0) {
      NodeRef &rnode(trace.current());
      keyappend(trace);
      rnode.child_last(rnode.node->children_[0], trace);
    } else {
      trace.parent_prev();
    }
  }

  void reinsert(TempCompressed &rest_me, trieindex_t index, Trace &trace) {
    BitTrie *bn;
    Page::ptr child;

    if (raw_size(rest_me.node()) == 0) {
      TESTPOINT(CompressReinsert0);
      child = trace.current().node->children_[0];
      trace.current().node->children_[0] = 0;
      bn = BitTrie::cast(trace.current().node);
      bn->add(index, child, trace.current().node->children_);
    } else {
      TESTPOINT(CompressReinsert1);
      rest_me.add_to(trace);
      child = trace.parent().node->children_[0];
      trace.parent().node->children_[0] = 0;

      Node *node = trace.parent().node;
      bn = BitTrie::cast(node);
      bn->add(index, trace.connect_ptr(), node->children_);
      trace.current().node->children_[0] = child;
      trace.pop();
    }
  }

  void add(const TempNode &leaf, Trace &trace) {
    NodeRef &rnode(trace.current());
    Slice key(trace.current_key());
    size_t len = raw_size(rnode);
    char *data = raw_data(rnode);

    if (!rnode.node->children_[0]) {
      // A new leaf with some rest key is inserted
      TESTPOINT(CompressAddNew);
      assert(len == key.size());
      leaf.add_to(trace);
      trace.parent().node->children_[0] = trace.connect_ptr();
      return;
    }

    size_t prefix_size =
        common_prefix(key.data(), data, std::min(len, key.size()));
    TempCompressed rest_me(
        Slice(data + prefix_size + 1, len - prefix_size - 1));
    TempTrie trie(1);

    trace.reserve_space(trie.pad_size());
    
    // rnode may have changed
    NodeRef& nrnode = trace.current();
    len = raw_size(nrnode);
    data = raw_data(nrnode);
    Page::ptr child = nrnode.node->children_[0];
    int delta;
    trieindex_t first = data[0];

    // the next lines will not change child, because we allready reserved
    // the space above

    if (prefix_size == 0) {
      // insert one trie node
      TESTPOINT(CompressAdd0);
      delta = trace.change_node(trie);
    } else {
      // another compressed
      TESTPOINT(CompressAdd1);
      first = data[prefix_size];

      Slice next_key(key.data(), prefix_size);
      delta = trace.change_node(TempCompressed(next_key));
      trie.add_to(trace);
      trace.parent().node->children_[0] = trace.connect_ptr();
    }

    if (child > nrnode.ptr())
      child += delta;

    // temporary put the child id on the current() trie
    // (ensures if current is moved also the child is moved)
    trace.current().node->children_[0] = child;
    reinsert(rest_me, first, trace);
    trace.current().add(leaf, trace);
  }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";
    size_t len_ = raw_size(node);
    char *data_ = raw_data(node);

    out << t3 << "type:  compressed" << std::endl;
    out << t3 << "data:  " << std::setw(2) << std::setfill('0')
        << (int)data_[0];
    for (size_t i = 1; i < len_; i++)
      out << "|" << std::setw(2) << std::setfill('0') << (int)data_[i];
    out << std::endl;
    out << t3 << "size:  " << (int)len_ << std::endl
        << t3 << "child: " << (int)node->children_[0] << std::endl;
  }
#endif
};

static CompressedHandler compressed;

// links to another page

struct LinkHandler : public LeafBase {
  inline PageLink *raw_data(const Node *node) {
    return (PageLink *)(((char *)node) + sizeof(Node::header_t));
  }

  void data(Node *node, const void *data) {
    memcpy(raw_data(node), data, sizeof(PageLink));
  }

  Slice data(const Node *node) {
    return Slice((char*)raw_data(node), sizeof(PageLink));
  }

  NodeRef &link_node(Trace &trace) {
    NodeRef &rnode(trace.current());
    PageLink *link = raw_data(rnode.node);
    PageRef next_page(trace.map.get_page(link->page_id));
    return trace.push(
        NodeRef(next_page, next_page.page->entry_points[link->entry] - 1));
  }

  void find(Trace &trace) { link_node(trace).find(trace); }

  void first(Trace &trace) { link_node(trace).first(trace); }

  void last(Trace &trace) { link_node(trace).last(trace); }

  void add(const TempNode &leaf, Trace &trace) {
    assert(0); // may never be called
  }

  bool eat_child(NodeRef &rnode, Node *data) { return false; }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";
    PageLink *link = raw_data(node);
    out << t3 << "type:  link" << std::endl
        << t3 << "page:  " << link->page_id << std::endl
        << t3 << "entry: " << (int)link->entry << std::endl;
  }
#endif
};

static LinkHandler link;

struct TrieHandler : public TrieBase {
  size_t count(const Node *node) { return 65; }

  void find(Trace &trace) {
    Slice key(trace.current_key());
    NodeRef &rnode(trace.current());
    int index = key.empty() ? 0 : key[0] + 1;
    Page::ptr child = rnode.node->children_[index];
    if (child)
      rnode.child_find(child, trace);
  }

  void first(Trace &trace) {
    NodeRef &rnode(trace.current());
    for (int index = 0; index < 65; index++) {
      Page::ptr child = rnode.node->children_[index];
      if (child) {
        if (index)
          trace.key.push_back((char)index - 1);
        rnode.child_first(child, trace);
        return;
      }
    }
    assert(0);
  }

  void last(Trace &trace) {
    NodeRef &rnode(trace.current());
    for (int index = 64; index >= 0; index--) {
      Page::ptr child = rnode.node->children_[index];
      if (child) {
        if (index)
          trace.key.push_back((char)index - 1);
        rnode.child_last(child, trace);
        return;
      }
    }
    assert(0);
  }

  void next(Trace &trace) {
    NodeRef &rnode(trace.current());
    Slice key(trace.current_key());
    int index = key.empty() ? 1 : key[0] + 2;
    for (; index < 65; index++) {
      Page::ptr child = rnode.node->children_[index];
      if (child) {
        trace.cut_key();
        trace.key.push_back((char)index - 1);
        rnode.child_first(child, trace);
        return;
      }
    }

    trace.parent_next();
  }

  void prev(Trace &trace) {
    NodeRef &rnode(trace.current());
    Slice key(trace.current_key());
    
    if (key.empty()) {
      trace.parent_prev();
      return;
    }

    int index = ((int)key[0]);
    for (; index >= 0; index--) {
      Page::ptr child = rnode.node->children_[index];
      if (child) {
        trace.cut_key();
        if (index)
          trace.key.push_back((char)index - 1);
        rnode.child_last(child, trace);
        return;
      }
    }
    trace.parent_prev();
  }

  void add_node(NodeRef &rnode, trieindex_t index, const TempNode &node,
                Trace &trace) {
    node.add_to(trace);
    rnode.node->children_[index + 1] = trace.connect_ptr();
  }

  bool remove_child(Trace &trace) {
    NodeRef &rnode(trace.current());
    Slice key(trace.current_key());
    Page::ptr *children = rnode.node->children_;

    int index = key.empty() ? 0 : key[0] + 1;
    children[index] = 0;

    size_t count = 0;

    for (int i = 1; i < 65; i++) {
      if (children[i])
        count++;
    }

    if (count == 60) {
      TESTPOINT(TrieRemove);
      TempTrie trie(count);
      Node *node = trie.node();
      node->children_[0] = children[0];
      BitTrie *bt = BitTrie::cast(node);
      for (int i = 1; i < 65; i++) {
        if (children[i])
          bt->add(i - 1, children[i], node->children_);
      }
      trace.change_node(trie);
    }

    return true;
  }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";

    out << t3 << "type: trie" << std::endl;
    out << t3 << "size: " << node->size << std::endl;
    out << t3 << "data: ";

    if (node->children_[0]) {
      out << "E>" << (int)node->children_[0];
      out << "|";
    }

    for (size_t i = 1; i < 65; i++) {
      if (i != 1)
        out << "|";

      out << std::setw(2) << std::setfill('0') << i - 1 << ">"
          << (int)node->children_[i];
    }

    out << std::endl;
  }
#endif
};

static TrieHandler trie;

struct BitTrieHandler : public TrieBase {
  size_t count(const Node *node) { return BitTrie::cast(node)->count() + 1; }

  boost::uint64_t start_grow(const Node *node) {
    return BitTrie::cast(node)->bits;
  }

  void end_grow(Node *node, boost::uint64_t value) {
    BitTrie::cast(node)->bits = value;
  }

  void find(Trace &trace) {
    NodeRef &rnode(trace.current());
    BitTrie *trie = BitTrie::cast(rnode.node);
    Slice key(trace.current_key());

    if (key.empty()) {
      if (rnode.node->children_[0])
        rnode.child_find(rnode.node->children_[0], trace);
      return;
    }

    trieindex_t index = key[0];
    int child_index = trie->get_child_index(index);
    if (child_index >= 1)
      rnode.child_find(rnode.node->children_[child_index], trace);
  }

  void first(Trace &trace) {
    NodeRef &rnode(trace.current());
    BitTrie *trie = BitTrie::cast(rnode.node);

    if (rnode.node->children_[0]) {
      rnode.child_first(rnode.node->children_[0], trace);
    } else {
      int index = trie->first_bit();
      assert(index >= 0);
      trace.key.push_back((char)index);
      int child_index = trie->get_child_index(index);
      rnode.child_first(rnode.node->children_[child_index], trace);
    }
  }

  void last(Trace &trace) {
    NodeRef &rnode(trace.current());
    BitTrie *trie = BitTrie::cast(rnode.node);
    int child_index = 0;
    int index = trie->last_bit();
    if (index >= 0) {
      trace.key.push_back((char)index);
      child_index = trie->get_child_index(index);
    }
    assert(rnode.node->children_[child_index]);
    rnode.child_last(rnode.node->children_[child_index], trace);
  }

  void next(Trace &trace) {
    NodeRef &rnode(trace.current());
    BitTrie *trie = BitTrie::cast(rnode.node);
    Slice key(trace.current_key());
    int index = key.empty() ? trie->first_bit() : trie->next_bit(key[0]);

    if (index >= 0) {
      trace.cut_key();
      trace.key.push_back((char)index);
      int child_index = trie->get_child_index(index);
      assert(rnode.node->children_[child_index]);
      rnode.child_first(rnode.node->children_[child_index], trace);
      return;
    }

    trace.parent_next();
  }

  void prev(Trace &trace) {
    NodeRef &rnode(trace.current());
    BitTrie *trie = BitTrie::cast(rnode.node);
    Slice key(trace.current_key());

    if (key.empty()) {
      trace.parent_prev();
      return;
    }
    
    int child_index = 0;
    int index = trie->prev_bit(key[0]);

    trace.cut_key();
    if (index >= 0) {
      trace.key.push_back((char)index);
      child_index = trie->get_child_index(index);
      assert(rnode.node->children_[child_index]);
    }

    if (rnode.node->children_[child_index])
      rnode.child_last(rnode.node->children_[child_index], trace);
    else
      trace.parent_prev();
  }

  void add_node(NodeRef &rnode, trieindex_t index, const TempNode &node,
                Trace &trace) {
    BitTrie *bittrie = BitTrie::cast(rnode.node);
    size_t count = bittrie->count();

    if (count == 60) {
      // change to Trie the space is the same as BitTrie
      TESTPOINT(BitTrieAdd0);
      TempTrie trie(count + 1);

      Node *parent = trie.node();
      parent->children_[0] = rnode.node->children_[0];

      int bit = bittrie->first_bit(), i = 1;
      while (bit >= 0) {
        parent->children_[bit + 1] = rnode.node->children_[i];
        bit = bittrie->next_bit(bit);
        i++;
      }
      trace.change_node(trie);
      trace.add(node);

      parent = trace.parent().node;
      parent->children_[index + 1] = trace.connect_ptr();
      return;
    }

    trace.grow_node_by(sizeof(Page::ptr));
      
    TESTPOINT(BitTrieAdd1);
    node.add_to(trace);
    Node *parent = trace.parent().node;
    bittrie = BitTrie::cast(parent);
    bittrie->add(index, trace.connect_ptr(), parent->children_);
  }

  bool remove_child(Trace &trace) {
    NodeRef &rnode(trace.current());
    Slice key(trace.current_key());
    BitTrie *bittrie = BitTrie::cast(rnode.node);
    int index = key.empty() ? -1 : key[0];

    if (index < 0) {
      TESTPOINT(BitTrieRemove0);
      rnode.node->children_[0] = 0;
    } else {
      TESTPOINT(BitTrieRemove1);
      bittrie->remove(index, rnode.node->children_);
      trace.grow_node_by(-(int)sizeof(Page::ptr));
    }

    return bittrie->count() > 0 || rnode.node->children_[0];
  }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";

    BitTrie *bittrie = BitTrie::cast(node);

    out << t3 << "type: bittrie" << std::endl;
    out << t3 << "size: " << node->size << std::endl;
    out << t3 << "data: ";

    if (node->children_[0]) {
      out << "E>" << (int)node->children_[0];

      if (bittrie->count())
        out << "|";
    }

    int bit = bittrie->first_bit(), i = 1;
    while (bit >= 0) {
      if (i != 1)
        out << "|";
      out << std::setw(2) << std::setfill('0') << bit << ">"
          << (int)node->children_[i];
      bit = bittrie->next_bit(bit);
      i++;
    }

    out << std::endl;
  }
#endif
};

static BitTrieHandler bittrie;

// a leaf node
struct LeafHandler : public LeafBase {
  inline char *raw_data(const Node *node) {
    return ((char *)node) + sizeof(Node::header_t);
  }

  inline size_t raw_size(const Node *node) {
    return node->size - sizeof(Node::header_t);
  }

  void data(Node *node, const void *data) {
    memcpy(raw_data(node), data, raw_size(node));
  }

  Slice data(const Node *node) { return Slice(raw_data(node), raw_size(node)); }

  void find(Trace &trace) {}

  void prev(Trace &trace) {
    Slice key(trace.current_key());
    if (key.empty())
      trace.parent_prev();
    else
      trace.cut_key();
  }

  void add(const TempNode &leaf, Trace &trace) {
    TESTPOINT(LeafAdd0);
    Slice key(trace.current_key());
    assert(key.size() != 0);
    TempLeaf me(trace.current().data());
    trace.change_node(TempTrie());
    // reinsert me to trie
    me.add_to(trace);
    trace.parent().node->children_[0] = trace.connect_ptr();
    trace.pop(); // back to parent
    trace.add(leaf);
  }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";

    std::string data(node->data().string());
    if (node->type == kLeaf) {
      out << t3 << "type:  leaf" << std::endl
          << t3 << "data:  " << data << std::endl
          << t3 << "size:  " << data.size() << std::endl;
    } else {
      out << t3 << "type:  bigleaf" << std::endl;
    }
  }
#endif
};

static LeafHandler leaf;

struct RemovedHandler : public NodeHandler {
  size_t len(const Node *node) {
    assert(0);
    return 0;
  }

  void data(Node *node, const void *data) { assert(0); }

  Slice data(const Node *node) {
    assert(0);
    return Slice();
  }

  size_t count(const Node *node) { return 0; }

  void find(Trace &trace) { assert(0); }
  void next(Trace &trace) { assert(0); }
  void prev(Trace &trace) { assert(0); }
  void first(Trace &trace) { assert(0); }
  void last(Trace &trace) { assert(0); }
  void add(const TempNode &leaf, Trace &trace) { assert(0); }

#ifdef DEBUG
  void dump(Page *page, Node *node, std::ostream &out) {
    const char *t3 = "            ";
    out << t3 << "type:  removed" << std::endl
        << t3 << "size:  " << node->size << std::endl;
  }
#endif
};

static RemovedHandler removed;

NodeHandler *NodeHandler::handlers[7] = {&leaf, &leaf,    &link,   &compressed,
                                         &trie, &bittrie, &removed};

void TempNode::to_leaf(const Slice &value) {
  size_t size = value.size();
  Page::ptr ptr = page.new_node(sizeof(Node::header_t) + size);
  Node *node = page.node(ptr);
  node->type = kLeaf;
  node->data(value.data());
}

void TempNode::to_trie(size_t child_count) {
  if (child_count > 60) {
    Page::ptr ptr = page.new_node(sizeof(Node::header_t) + sizeof(Page::ptr) * 65);
    Node *node = page.node(ptr);
    node->type = kTrie;
  } else {
    // +1 bacause of end child
    size_t size = sizeof(BitTrie) + (child_count + 1) * sizeof(Page::ptr);
    Page::ptr ptr = page.new_node(sizeof(Node::header_t) + size);
    Node *node = page.node(ptr);
    node->type = kBitTrie;
  }
}

void TempNode::to_compressed(const Slice &part) {
  size_t size(part.size());
  Page::ptr ptr = page.new_node(sizeof(Node::header_t) + sizeof(Page::ptr) + size); // one child
  Node *node = page.node(ptr);
  node->type = kCompressed;
  node->data(part.data());
}

void TempNode::to_link(pageid_t page_id, Page::entry_t entry) {
  PageLink link;
  link.page_id = page_id;
  link.entry = entry;
  Page::ptr ptr = page.new_node(sizeof(Node::header_t) + sizeof(link));
  Node *node = page.node(ptr);
  node->type = kLink;
  node->data(&link);
}

void TempNode::add_to(Trace &trace) const {
  if (type() == kLink) {
    PageLink *link = (PageLink *)node()->data().data();
    if (link->page_id == trace.current().page.id) {
      PageRef &page = trace.current().page;
      NodeRef dst(page, page.page->entry_points[link->entry] - 1);
      page.page->entry_points[link->entry] = 0;
      trace.push(dst);
    }
  }

  size_t size_ = pad_size();
  trace.reserve_space(size_);
  Page::ptr new_ptr = trace.current().page.new_node(size_);
  NodeRef dst(trace.current().page, new_ptr);
  memcpy(dst.node, node(), size_);
  trace.push(dst);
}

} // namespace larch_leaves
