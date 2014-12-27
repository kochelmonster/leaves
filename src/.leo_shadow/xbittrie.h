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


struct NodeHandler {
  // finds key in trie, walks until the leaf was found or to the
  // last fitting note. context.trace will be filled on the way
  virtual void find(const Slice& key, NodeRef& rnode, Trace& trace) = 0;
  virtual void next(NodeRef& rnode, Trace& trace) = 0;
  virtual void prev(NodeRef& rnode, Trace& trace) = 0;
  virtual void first(NodeRef& rnode, Trace& trace) = 0;
  virtual void last(NodeRef& rnode, Trace& trace) = 0;
  virtual void add(const Slice& key, const Slice& value, 
                   NodeRef& rnode, Trace& trace) = 0;
  // remove the traces last_index
  virtual bool remove_last_index(NodeRef& rnode, Trace& trace) {
      return false;
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
