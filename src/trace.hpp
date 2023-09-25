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

  Node* get_node() { return page->get_node(nid); }
  const Node* get_node() const { return page->get_node(nid); }

  node_p* get_ie() const { return page->get_ie(nid); }
};

struct Trace {
  Trace(Storage& storage_)
      : storage(storage_), transaction_id(0), txn_root(nullptr) {
    view = storage.view;
    current_key.reserve(1024);
    stack.resize(128);
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
    const Transition& back = stack_back();
    return NodeHandler::HANDLERS[back.page->get_ie(back.nid)->type];
  }

  void push_to_stack(stored_ptr link) {
    Transition& back = push_stack();
    back.page = link.get<Page>(view.get());
    back.pspage.val = link.val;
    back.nid = 0;
    back.key_pos = current_key.size();
  }

  void push_to_stack(Page* page) {
    Transition& back = push_stack();
    back.page = page;
    back.pspage.val = 0;
    back.nid = 0;
    back.key_pos = current_key.size();
  }

  void push_to_stack(node_t nid) {
    Transition& pback = stack_back();
    Transition& back = push_stack();
    back.page = pback.page;
    back.pspage.val = pback.pspage.val;
    back.nid = nid;
    back.key_pos = current_key.size();
  }

  Transition& push_stack() {
    if (stack.size() <= stack_size) {
      stack.resize(stack_size+100);
    }
    return stack[stack_size++];
  }

  Transition& stack_back() {
    return stack[stack_size-1];
  }

  const Transition& stack_back() const {
    return stack[stack_size-1];
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
  size_t stack_size;
  std::string current_key;
};

}  // namespace leaves

#endif  // _LEAVES_TRACE_HPP