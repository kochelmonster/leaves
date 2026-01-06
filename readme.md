leaves:    version 3.0
Date:           Mon Sep 25 10:18:31 2023
CPU:            20 * 12th Gen Intel(R) Core(TM) i7-12700KF
CPUCache:       25600 KB
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    1000000
RawSize:    110.6 MB (estimated)
FileSize:   62.9 MB (estimated)
------------------------------------------------
fillseq      :       5.744 micros/op;   19.3 MB/s
fillseqsync  :       4.596 micros/op;   24.1 MB/s (10000 ops)
fillrandsync :       5.359 micros/op;   20.6 MB/s (10000 ops)
fillrandom   :       8.563 micros/op;   12.9 MB/s
overwrite    :       9.226 micros/op;   12.0 MB/s
readrandom   :       0.359 micros/op;  229.4 MB/s
fillrand100K :      25.866 micros/op; 3687.6 MB/s (1000 ops)
fillseq100K  :      29.313 micros/op; 3253.9 MB/s (1000 ops)
readrand100K :       1.408 micros/op; 23345.6 MB/s


Roadmap:

- big values / big keys
- (x) remove burst code
- (x) delete records 
- (x) Multidb (storage -> db -> cursor)
- Tools (Multithread)   <-- Sonntag
- Merge
- LIRS Caching / WASM
- Drivers (MySQL/Mongo)
- Replication / Merkle Tries

---

for(auto iter = fcursor.being(); iter != fcursor.end(); iter++) {
    if (*iter == "ll")
        iter.use();
}



TransferTrie:
 Ein continuierlicher Speicher
 Ein Query Trie 


- COW Traits: for switching cow on and off
- Eigener memmanger für höhere nodes (level < 3) für bessere Lokalität?


Trie Filter (Query Trie): nur values mit dem filter werden zurückgegeben

move (cursor1, cursor2):
   // wenn in der gleichen databank wir einfach nur node umgehängt

BlockHeader::free_idx not used

There is a major flaw:
You can find native benachmarks for leaves, lmdb and rocksdb
the sources are in 
/home/michael/src/leaves/benchmarks

the compilded files in 
/home/michael/src/leaves/build

here is a call excerpt from terminal

.venv) michael@EliteMini:~/src/leaves$ /home/michael/src/leaves/build/db_bench_leaves --batch_size=1 --benchmarks=fillrandom
Date:           Wed Oct 22 19:09:15 2025
CPU:            16 * AMD Ryzen 7 8745H w/ Radeon 780M Graphics
CPUCache:       1024 KB
Storage:     MapStorage
Keys:        16 bytes each
Values:      100 bytes each (50 bytes after compression)
Entries:     1000000
RawSize:     110.6 MB (estimated)
FileSize:    62.9 MB (estimated)
------------------------------------------------
fillrandom   :       1.038 micros/op;  106.6 MB/s     
(.venv) michael@EliteMini:~/src/leaves$ /home/michael/src/leaves/build/db_bench_mdb --batch_size=1 --benchmarks=fillrandom --wmap
MDB:    version LMDB 0.9.31: (July 10, 2023)
Date:           Wed Oct 22 19:09:32 2025
CPU:            16 * AMD Ryzen 7 8745H w/ Radeon 780M Graphics
CPUCache:       1024 KB
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    1000000
RawSize:    110.6 MB (estimated)
FileSize:   62.9 MB (estimated)
------------------------------------------------
fillrandom   :       1.576 micros/op;   70.2 MB/s     
(.venv) michael@EliteMini:~/src/leaves$ /home/michael/src/leaves/build/db_bench_rocksdb --benchmarks=fillrandom 
RocksDB:    version 8.9.1
Date:           Wed Oct 22 19:09:47 2025
CPU:            16 * AMD Ryzen 7 8745H w/ Radeon 780M Graphics
CPUCache:       1024 KB
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    1000000
RawSize:    110.6 MB (estimated)
FileSize:   62.9 MB (estimated)
------------------------------------------------
fillrandom   :       5.524 micros/op;   20.0 MB/s   

as you see leaves is much faster than lmdb and rocksdb


Read the code and understand how leaves works.
We try to optimize inserting by using an lsm mechanism.

- Inserts will not be directly written to the Persistent Storage (_MemoryMapedFile, _FileStore) but first to a _MemoryDB without cow mechanism.
- When committed the _MemoryDB is inserted to a queue
- An extra merge thread will merge the _MemoryDBs from the queue to the Persistent Storage in the background.
- The _MemoryDB shall have a configurable maximum size (e.g. 16Kb).
- If the maximum size is reached the _MemoryDB is inserted to the queue and a new _MemoryDB is created for further inserts.
- In synch mode only the _MemoryDB needs to be immediatly flushed to the disk.
- bigvalues will still be written directly to the Persistent Storage.


As far as I can see theses changes have to be made:

- In the _DB class there must have support for the _MemoryDB queue.

- For the _MemoryMappedFile Storage the _MemoryDB must resident in the same memory mapped file, so all process can access them, while they are still in the queue.
- A new _Cursor Wrapper struct has to be defined, that has the same Interface like cursor, but is able to handle
multiple cursors to the _MemoryDB's and a cursor to the Persistent Storage.
- The _Cursor Wrapper must also handle that, when _MemoryDB's are merged to the Persistent Storage, the cursors are still valid.

1. What is your opinion about this approach?
2. Did I forget something important?



Answers to your questions about Missing Pieces.
- A search has to be ordered from the newst _MemoryDB to the oldest and finally to the Persistent Storage.
  the first found key is returned.
- Delete operations have to be stored in the _MemoryDB as tombstone entries.
- They will be pruned during a backgound process. That means cursors have to handle that tombstone entries are skipped.
- As you correctly suggested MemoryDB must be refcounted before they can be deleted.
- The spaces allocation and the queue flush shall stored inside _Transaction. In case of a rollback any values are simply reset. 
- The transaction gets a new merge phase after the commit. The transaction is finally done after the merge is finished. 
- If the process crashes before the merge is done, the merge is done during the next start of the database.
- The merge starts with the commit.
- The MemoryDB are allocated in extra Areas. The reference count is not for single _MemoryDBs but the Area.
- If the area reference count is zero the area can be deleted.
- MemoryDBs are merged from oldest to newest. Merging mulitple DB's at once woud be nice but I suspect the merge algorithm gets too complicated. (see _merger.hpp_)
- If not in synch mode, the data is lost during a crash before the merge is done.

Additional advantage of this approach, would be the support of multiple writers.

electra, julius, arachne, kaskade, lars, polaris, peppy, augustus, yasmin ishmael Kayla

Bigmemory optimizations:
- BigMemory only size_cursor
  - BigMemory chunk get a header of SizeKey 
  - sizekey has a next flag (to signal the memory behind chunk is also a big memem chunk)
  - in defrag the next chunk's header is checked and if it exists in the freelest the chunk is merged

Locality optimizations:
- BlockHeader bekommt ein uin16_t size flag
- Nodes werden nicht von BlockHeader abegeleited, sondern haben nur noch ein offset zum BlockHeader
  node_pointer - offset is the BlockHeader pointer
- Offset bekommen ein relative flag, mit diesem flag wird der offset als int64 relativ zur akutellen addresse interpretiert
- Transistion::update  nur wenn node::offset == 0 (root innerhalb eines blocks) dann kopieren des ganzen blocks
- Inserter alloc und create in zwei verschienden phasen.

