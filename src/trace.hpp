#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>
#include "storage.hpp"
#include "page.hpp"

namespace leaves {


struct Trace;


struct Transition {
  location_p pnode;  // Position to the node 
  location_p plink;  // Position to the link of the node
  Transition(location_p pnode_, location_p plink_): pnode(pnode_), plink(plink_) {}
};


struct Location  {
  location_p loc;
  Page* page;
  Node* node;
  
  Location(location_p loc_, Page* page_, Node* node_) : loc(loc_), page(page_), node(node_) {}

  Transition next(const node_p* link) const {
    return Transition(loc.replace(*link), loc.replace(page->offset(link)));
  }

  Transition next(const location_p* link) const {
    return Transition(*link, loc.replace(page->offset(link)));
  }
};


struct Trace {
  Trace(Storage& storage) 
      : storage(storage), 
        transaction_id(storage.transaction_id()), 
        writing(false) {
    current_key.reserve(1024);
    stack.reserve(128);
  }

  bool valid() const { return handler()->valid(*this); }
  void find(const Slice& key);
  void first();
  void last();
  void next();
  void prev();
  void set_value(const Slice& value);
  Slice get_value() const;
  void remove();
  void commit();

  NodeHandler* handler() const {
    return NodeHandler::HANDLERS[stack.back().pnode.type];
  }

  
  void change_type(uint16_t type);
  void push_back(const Transition& trans);
  Location back() const;

  Storage& storage;
  std::vector<Transition> stack;
  Slice rest_key;
  std::string current_key;
  uint64_t transaction_id;
  bool writing;
};

inline void Trace::push_back(const Transition& trans) {
  return stack.push_back(trans);
}
  
inline Location Trace::back() const {
  const Transition& last = stack.back();
  Page* page = storage.page(last.pnode, writing);
  Node* node = page->node(last.pnode.node);
  return Location(last.pnode, page, node);
}

inline void Trace::change_type(uint16_t type) {
  Transition& last = stack.back();
  Page* p = storage.page(last.plink, true);
  ((node_p*)&p->content[last.plink.offset])->type 
      = last.plink.type = last.pnode.type = type;
  if (last.pnode.node.offset == 0) {
    p = storage.page(last.pnode, true);
    p->end.type = type;
  }
}

} // namespace leaves







#endif // _LEAVES_TRACE_HPP