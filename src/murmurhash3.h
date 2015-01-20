
#ifndef MURMURHASH3_H
#define MURMURHASH3_H

#include <stdlib.h>
#include <boost/cstdint.hpp>

boost::uint32_t calc_hash(const char* data, size_t len);

#endif

