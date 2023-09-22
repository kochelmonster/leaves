#ifndef _LEAVES_TRACE_HPP
#define _LEAVES_TRACE_HPP
#include <leaves.hpp>

#include "page.hpp"
#include "storage.hpp"

namespace leaves {

struct Trace;

struct Transition {
  Page* page;
  stored_ptr pspage;
  node_t nid;
  size_t key_pos;

  Transition() : page(0), nid(0), key_pos(0) {
    pspage.val = 0;
  }

  Transition(Page* page_, stored_ptr pspage_, node_t nid_, size_t key_pos_)
      : page(page_), pspage(pspage_), nid(nid_), key_pos(key_pos_) {}

  Node* get_node() { return page->get_node(nid); }
  const Node* get_node() const { return page->get_node(nid); }

  node_p* get_ie() const { return page->get_ie(nid); }
};

struct Trace {
  Trace(Storage& storage_)
      : storage(storage_), transaction_id(0), txn_root(nullptr) {
    view = storage.view;
    current_key.reserve(1024);
    stack.reserve(128);
  }

  ~Trace();

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
  void rollback();
  void go_back(int index);

  // if view has changed reload the stack
  void reload();

  // refresh the find position
  void refresh();

  NodeHandler* handler() const {
    const Transition& back = stack.back();
    return NodeHandler::HANDLERS[back.page->get_ie(back.nid)->type];
  }

  void push_to_stack(stored_ptr link) {
    Page* page = link.get<Page>(view.get());
    stack.push_back(Transition(page, link, 0, current_key.size()));
  }

  void push_to_stack(Page* page) {
    Transition& back = stack.back();
    stack.push_back(Transition(page, stored_ptr(), 0, current_key.size()));
  }

  void push_to_stack(node_t nid) {
    Transition& back = stack.back();
    stack.push_back(
        Transition(back.page, back.pspage, nid, current_key.size()));
  }

  void release_transaction_view();
  void update_transaction_view();

  bool split(Page* page);

  Storage& storage;
  uint64_t transaction_id;
  stored_ptr root;
  Page* txn_root;
  MemoryView_ptr view;
  std::vector<Transition> stack;
  Slice rest_key;
  std::string current_key;
};

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP