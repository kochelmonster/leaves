#include <boost/interprocess/detail/atomic.hpp>

#include <trace.hpp>

using namespace boost::interprocess;

namespace leaves {

Trace::~Trace() {
  release_transaction_view();
}


void Trace::refresh() {
  std::string key = current_key;
  key.append(rest_key.data(), rest_key.size());
  find(key);
}

void Trace::release_transaction_view() {
  int idx = transaction_id % TRANSACTION_COUNT;
  const Header* header = storage.get_header();
  const Transaction &old = header->txn[idx];
  if (old.id == transaction_id) {
    ipcdetail::atomic_dec32(&storage.shared->txn_ref_count[idx]);
  }
  transaction_id = 0;
}

void Trace::update_transaction_view() {
  if (transaction_id != storage.transaction_id()) {
    release_transaction_view();
    transaction_id = storage.transaction_id();
    int idx = transaction_id % TRANSACTION_COUNT;
    ipcdetail::atomic_inc32(&storage.shared->txn_ref_count[idx]);
    root = view->get_header()->txn[idx].root;
  }
}

void Trace::reload() {
  storage.check_size();
  if (view != storage.view) {
    view = storage.view;
    MemoryView* view_ = view.get();
    auto last = stack.begin();
    for (auto i = stack.begin(); i != stack.end(); i++) {
      if (i->pspage.val)
        i->page = i->pspage.get<Page>(view_);
    }
  }
}

void Trace::find(const Slice& key) {
  rest_key = key;
  current_key.clear();
  stack.clear();
  update_transaction_view();
  reload();

  if (txn_root)
    push_to_stack(txn_root);
  else 
    push_to_stack(root);

  while (handler()->find(*this))
    ;
}

void Trace::set_value(const Slice& value) {
  if (storage.start_transaction()) {
    assert(!txn_root);
    txn_root = storage.get_writable_page(stack.begin()->pspage);
  }

  Page *page = txn_root;
  int last_page_change = 0;
  // change pages to writable pages
  for (auto i = stack.begin(); i != stack.end(); i++) {
    if (i->pspage.val) {
      i->pspage.val = 0;
      i->page = page;
    }
    node_p *pie = i->get_ie();
    if (pie->type == kLink) {
      last_page_change = i - stack.begin();
      pie->type = kHeapLink;
      Node* node = i->page->get_node(pie);
      page = node->pointer = storage.get_writable_page(node->link);
    } else if (pie->type == kHeapLink) {
      Node* node = i->page->get_node(pie);
      page = node->pointer;
    }
  }

  while (split(page)) {
    // reload the last page in the stack
    go_back(last_page_change);
    while (handler()->find(*this))
      ;
  }

  handler()->insert(*this, value);
  
  while (handler()->find(*this))
    ;
}

bool Trace::split(Page* page) {
  bool need_reload = false;
  while (!page->reserve(Page::MIN_SPACE, Page::MIN_COUNT)) {
    page->split(storage);
    need_reload = true;
  }
  return need_reload;
}

void Trace::go_back(int index) {
  stack.resize(index+1);
  Transition back = stack.back();
  int delta = current_key.size() - back.key_pos;
  rest_key = Slice(rest_key.data()-delta, rest_key.size()+delta);
  current_key.resize(back.key_pos);
}

Slice Trace::get_value() const {
  if (valid()) {
    const Transition& back = stack.back();
    assert(back.get_ie()->type == kValue);
    return view->get_value(back.get_node()->value.value);
  }
  return Slice();
}

void Trace::remove() {}


void Trace::rollback() {
  storage.rollback();
  txn_root->free_page(storage);
  txn_root = nullptr;
  refresh();
}

void Trace::commit() {
  storage.prepare_commit(txn_root->write_page(storage));
  storage.commit();
  txn_root = nullptr;
  refresh();
}

void Trace::first() {}

void Trace::last() {}

void Trace::next() {}

void Trace::prev() {}

}  // namespace leaves