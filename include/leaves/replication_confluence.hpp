#ifndef _LEAVES_REPLICATION_CONFLUENCE_HPP
#define _LEAVES_REPLICATION_CONFLUENCE_HPP

#include "confluence.hpp"
#include "intern/replication/_replication_db.hpp"

namespace leaves {

// Convenience aliases for the mmap + replication combination.
using MapReplicationConfluenceDB =
    ConfluenceDB<MapStorage, _DefaultConflictPolicy, _ReplicationDB>;
using MapReplicationConfluenceCursor =
    ConfluenceCursor<MapStorage, _DefaultConflictPolicy, _ReplicationDB>;

}  // namespace leaves

#endif  // _LEAVES_REPLICATION_CONFLUENCE_HPP
