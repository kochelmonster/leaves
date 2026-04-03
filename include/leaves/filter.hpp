#ifndef _LEAVES_FILTER_HPP
#define _LEAVES_FILTER_HPP

#include "intern/filter/_range_filter.hpp"

namespace leaves {

using RangeBound = _RangeBound;

template <typename DB, typename Executor>
using RangeFilter = _RangeFilter<DB, Executor>;

}  // namespace leaves

#endif  // _LEAVES_FILTER_HPP
