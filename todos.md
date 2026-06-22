- nimm claude um WAL auf echtes ACID zu überprüfen

kv_browser:
1. Soll javascript client von js verwenden
2. Javascript API for replication
3. type mismatch fehler bei jedem refrsh ist falsch





In einem eigenen Dokument? browser api, repl

- Replication Documentation
-current_wire_root -> wired_delete_root?
- keine transaction when delete::_new_leaves empty?


- TCP Replication Example

Readme:
-  Update ConfluenceDB (experimental)
- Cursor.reserve(10) API
- Wie wird leaves in andere Projekte eingebunden? (z.B. YCSB) (über submodule, + install?)


- ycsb-Readme update: alle Änderungen


browser example endlich laufend

browser hat kein wal

browser benchmark

was ist mit ws_replication? in tests?

-sJSPI -> -sASYNCIFY!


_db code functions kürzen


Nochmal wegen scheiß Ki:
## wal ist nicht multiprocess fähig
In MemoryMapped Storage wal must run in a multiprocess environment
- _write_off,  _next_log, _active_log, _last_commit[2] must be in the DB Header
- _wa_open must be protected by _storage.file_lock
Because wal is manipulated within a transaction it is guaranteed that only one process can write to the wal

## commit recovery
- wal_recover may not replay the whole log but must begin with with the first transaction > read_txn.
- if the last transaction in the wal is not commited, wal_recover must replay it but not commit it.
- In sanitze after wal_recover. If prepared_txn != _read_txn, the last transaction was not commited. 
  The transacton must be restored, i.e. txn_lock must be locked, active_txn must be set. 

## wal reset
- at the end of snatiation a _storage.flush(true, true) must be called and than the wal files deleted.



