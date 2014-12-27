#ifndef _LARCH_LEAVES_BITTRIE_H
#define _LARCH_LEAVES_BITTRIE_H

#include "larch/nodes.h"

namespace larch_leaves {

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

} // namespace larch_leaves 
#endif // _LARCH_LEAVES_BITTRIE_H
