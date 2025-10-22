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