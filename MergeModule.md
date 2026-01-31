Write a Merge module that merges trie a into trie b.
- take _Inserter as operation template
- create a depth first iterator that iterates through the trie a: merge-iter
- merge-iter uses the db of trie a for resolving offsets
- like _cursor, merge-iter saves a _current_key that represents the position of the current node in side the trie
- merge-iter uses a cursor of the destination db to find the corresponding destination node in the destination trie
- the merge process is like following
  1. check if the src-node has to split_compress the destination node
  2. enhance the trie part of node with the nodes of trie a and make a deep copy to trie b
- merge-iter continues with iterating through the trie childs thar are common in both tries

    



Write a Replicator
- breadth first search 


