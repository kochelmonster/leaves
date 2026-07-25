# Lessons Learned

This document captures key engineering lessons that shaped the current Leaves architecture.

## Data Locality
Several Attempts were made to get a better data locality for the trie nodes.


### Simple Hint

allocation hint for the memory pool. Try to find a block near to the parent node.
Result: There was no change in performance measurable.

### Node Cluster

When inserting a node cluster child nodes with their parent in a single memory block. 
Result: the performance descreased significanlty the cluster operation (particularly the extra memcpy work) were more expansive than the benefit of better locality. 

### Cluster Leaves

Neighboring leaves are clustered together in a single memory block. 
Result: Again the clustering operations were more expensive than the benefit of better locality. The performance descreased. Here especially page split with their extra memcpy work were the main reason.

## Multithread Hash Updater

removed because the performance gain did not justify the code complexity. (network transfer is the bottleneck)

## Big Values Write

In mmap better to write big value than to memcpy it

## Memory Pressure

In multiwriter mode


## Bigvalues in MMAP

- when the value size exceeds a certain threshold, it is faster to write the value directly to the mmap file instead of copying it into memory first. 
- The threshold is for every system different and must be calculated by benchmarking. 

## 256arry TrieNode
### branch key always in compressed
show exampe trie

## development

### printing trie structures eases development