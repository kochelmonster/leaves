#ifndef _LEAVES_REPLICATION_CONFLUENCE_HPP
#define _LEAVES_REPLICATION_CONFLUENCE_HPP

#include "confluence.hpp"
#include "intern/replication/_replication_db.hpp"

namespace leaves {

struct ConfluenceReplicationDB {
  template <typename Storage_>
  using DBWrapper = ConfluenceDB<Storage_, _ReplicationDB>;

  template <typename StorageImpl_>
  using DBImpl = _ReplicationDB<StorageImpl_>;
};

template <typename Traits>
class MapStorage_<Traits>::ConfluenceReplicationDB
  : public ::leaves::ConfluenceDB<MapStorage_<Traits>,
                  ::leaves::_ReplicationDB> {
 public:
  using Base = ::leaves::ConfluenceDB<MapStorage_<Traits>,
                                      ::leaves::_ReplicationDB>;
  template <typename AnyStorage>
  using DBWrapper = typename AnyStorage::ConfluenceReplicationDB;
  template <typename AnyStorageImpl>
  using DBImpl = ::leaves::_ReplicationDB<AnyStorageImpl>;

  ConfluenceReplicationDB() = default;
  using Base::Base;
};

}  // namespace leaves

#endif  // _LEAVES_REPLICATION_CONFLUENCE_HPP
