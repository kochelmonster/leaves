//@+leo-ver=4-thin
//@+node:michael.20141215222649.81:@shadow bittrie.h
//@@language cplusplus
//@@tabwidth -2
#ifndef _LARCH_LEAVES_BITTRIE_H
#define _LARCH_LEAVES_BITTRIE_H

//@<< includes >>
//@+node:michael.20141219202729.9:<< includes >>
#include "larch/nodes.h"
//@nonl
//@-node:michael.20141219202729.9:<< includes >>
//@nl

namespace larch_leaves {

//@+others
//@+node:michael.20141215222649.99:NodeHandler
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

//@-node:michael.20141215222649.99:NodeHandler
//@-others
} // namespace larch_leaves 
#endif // _LARCH_LEAVES_BITTRIE_H
//@nonl
//@-node:michael.20141215222649.81:@shadow bittrie.h
//@-leo
