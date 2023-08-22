#include "page.hpp"
#include "storage.hpp"
#include "trace.hpp"

namespace leaves {


uint16_t Page::alloc(Trace& trace, size_t size) {
  while (end.offset + size > sizeof(content)) {
    node_p root = node_p::b(0, end.type);
    move_node(trace, &root);
  }
  uint16_t result = end.offset;
  end.offset += size;
  return result;
}

void Page::scale_node(Trace& trace, size_t start, int delta)  {
  if (!delta) {
    return;
  }

  node_p root = node_p::b(0, end.type);

  if (delta > 0) {
    size_t move_size = end.offset - start;
    alloc(trace, delta);
    memmove(&content[start+delta], &content[start], move_size);
    if (!start)
      root.offset = delta;
  }
  else {
    end.offset += delta;
    memmove(&content[start], &content[start-delta], end.offset-start);
  }
    
  adjust_pointers(root, start, delta);
}

void Page::adjust_pointers(node_p npos, size_t start, int delta) {
  NodeHandler::HANDLERS[npos.type]->adjust_pointers(this, npos, start, delta);
}

bool Page::move_node(Trace& trace, node_p* pnode) {
  return NodeHandler::HANDLERS[pnode->type]->move_node(trace, this, pnode);
}


} // namespace leaves