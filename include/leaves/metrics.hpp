#ifndef _LEAVES_METRICS_HPP
#define _LEAVES_METRICS_HPP

#include "intern/db/_metrics_aspects.hpp"
#include "intern/storage/_mmap.hpp"
#include "intern/storage/_fstore.hpp"

namespace leaves {

// =============================================================================
// Convenience traits — drop-in replacements for MapTraits / _StoreTraits
// that enable all five metric mixins.
//
// Usage (map-backed storage):
//   auto storage = MapStorage_<MetricsMapTraits>::create("/path/to/db");
//   auto db = storage->open("mydb");
//   // ... use db normally ...
//   auto ops = db.aspect().ops_snapshot();
//   auto txns = db.aspect().txn_snapshot();
//
// Usage (file-backed storage):
//   FileStorage_<MetricsFileTraits> storage(...);
//
// Selective metrics — only pay for what you measure:
//   struct MyTraits : _MemoryMapTraits {
//     using Aspect = _TransactionAspect<_OperationAspect<>>;
//   };
// =============================================================================

struct MetricsMapTraits : _MemoryMapTraits {
  using Aspect = _AllMetricsAspect<>;
};

struct MetricsFileTraits : _StoreTraits {
  using Aspect = _AllMetricsAspect<>;
};

}  // namespace leaves

#endif  // _LEAVES_METRICS_HPP
