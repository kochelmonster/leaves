#ifndef _LARCH_LEAVES_BITTRIE_H
#define _LARCH_LEAVES_BITTRIE_H

#include "larch/nodes.h"

namespace larch_leaves {

// Interface for NodeRefs

typedef boost::uint8_t trieindex_t;


struct HandlerContext {
  Trace& trace;
  PageMap& map;
  HandlerContext(Trace& t, PageMap& m)
    : trace(t), map(m) {}
};


struct NodeHandler {
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(const Slice& key, NodeRef& rnode, 
                    HandlerContext& context) = 0;
  virtual void next(NodeRef& rnode, HandlerContext& context) = 0;
  virtual void prev(NodeRef& rnode, HandlerContext& context) = 0;
  virtual void first(NodeRef& rnode, HandlerContext& context) = 0;
  virtual void last(NodeRef& rnode, HandlerContext& context) = 0;
  virtual void pop(NodeRef& rnode, HandlerContext& context) = 0;
  virtual void add(const Slice& key, const Slice& value,
                   NodeRef& rnode, HandlerContext& context) = 0;
  virtual void remove(NodeRef& rnode, HandlerContext& context) = 0;

  void parent_next(NodeRef& rnode, HandlerContext& context) {
      pop(rnode, context);
      context.trace.nodes.back().next(context);
    }

  void parent_prev(NodeRef& rnode, HandlerContext& context) {
      pop(rnode, context);
      context.trace.nodes.back().prev(context);
    }

  static NodeHandler* handlers[200];
};

} // namespace larch_leaves 
#endif // _LARCH_LEAVES_BITTRIE_H
