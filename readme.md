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