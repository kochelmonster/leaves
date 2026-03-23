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

- synch mode in extra thread
- Merge
- LIRS Caching / WASM
- Drivers (MySQL/Mongo)
- Replication / Merkle Tries

---

TransferTrie:
 Ein continuierlicher Speicher
 Ein Query Trie 

Trie Filter (Query Trie): nur values mit dem filter werden zurückgegeben

move (cursor1, cursor2):
   // wenn in der gleichen databank wir einfach nur node umgehängt




Locality optimizations:
- BlockHeader bekommt ein uin16_t size flag
- Nodes werden nicht von BlockHeader abegeleited, sondern haben nur noch ein offset zum BlockHeader
  node_pointer - offset is the BlockHeader pointer
- Offset bekommen ein relative flag, mit diesem flag wird der offset als int64 relativ zur akutellen addresse interpretiert
- Transistion::update  nur wenn node::offset == 0 (root innerhalb eines blocks) dann kopieren des ganzen blocks
- Inserter alloc und create in zwei verschienden phasen.



change_leaf:
  the new trie and the two leaves in one block
  (big_key the first trie)

split_compressed:
  the lower trie with childen in one block
  the upper trie with its leaf in one block

- array extend all in one block


As you know the final goal is to save multiple nodes in one page.
The cow strategy (and also the locality goals) means that if a node inside a page is changed the whole page must be recreated: The new page will contain the changed node and all other nodes copied from the old page.
Does a standard proceeding exits to reach this goal?



    if (!is_page_root()) {
      // not a page root: cow has to be done in page root
      offset = parent().update(*this);
      return link();
    }

    // Compute PageHeader addresses from node pointers
    PageHeader* parent_header = (PageHeader*)((char*)node - sizeof(PageHeader));
    PageHeader* child_header =
        (PageHeader*)((char*)child.node - sizeof(PageHeader));
    if (!parent_header->needs_cow(*child_header)) {
      cursor->_db->make_dirty(node);
      return link();
    }

    // copy-on-write trie
    assert(is_trie());
    trie_ptr old_trie = trie();

    // copy whole page
    page_ptr old_page((char*)old_trie - sizeof(PageHeader));
    page_ptr new_page = cursor->_db->alloc_slot(old_page->slot_id);
    new_page->used = old_page->used;
    assert(new_page->used <= Traits::PAGE_SIZES[new_page->slot_id]);

    trie() = trie_ptr((TrieNode*)((char*)new_page + sizeof(PageHeader)));
    auto new_offset = cursor->_db->resolve(trie());
    memcpy((char*)trie(), (char*)old_trie, old_page->used);
    cursor->_db->free(old_page);
    assert(trie()->count() < trie()->MAX_BRANCH_COUNT);

    // Propagate COW upward: grandparent's needs_cow will compare its txn_id
    // with our NEW cloned trie's txn_id. If they match (same transaction), no
    // COW needed. If different, grandparent will also COW and recursively
    // propagate upward.
    if (!is_root()) offset = parent().update(*this);
    *offset = new_offset;

    // Return the child's offset location in the NEW cloned trie
    return link();



    Ich brauche einen Assistenten zur Job Suche. Die Funktion sollte in etwa folgende sein: - Ich lade meinen Resume hoch - Ich gebe Parameter ein (Gehaltsvorstellung, Location, Branche, Wünche usw.) Der Assistent durchkämmt regelmäßig das Internet, um passende Job Angebote zu finden (abgeglichen mit dem Resume, und den Parametern). Dabei konzentriert er sich direkt auf Firmen seiten und nicht auf Jobportale. Jeden Abend wird mir eine Email mit passenden Jobs gesendet mit einer kurzen Zusammenfassung. Ich kann dann auf einen Link in der Email klicken die direkt zu dem Angebot führt. Über den Link kann ich dann den Assistenten anweisen einen Cover letter zu schreiben, und wenn ein Fragebogen existiert diesen vorausfüllen. 1. Gibt es eine Webseite die diese Funktion anbietet?

    
- Browser version
- Convertsion tools value->binary sortable
- TransferTries (Result und Joins)
- Joiner
- Subtrie Replication
- Subtrie remove 
- multi thread merger


wasm
what is fake-indexeddb?