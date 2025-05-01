#ifndef _LEAVES__HASH_HPP
#define _LEAVES__HASH_HPP

struct NullHasher {
  typedef uint8_t hash_t[0];

  NullHasher() {}
  void update(const void* data, size_t size) {}
  void finalize(hash_t hash) {}
};


#if defined(BLAKE3_API)

struct Hasher {
  typedef uint8_t hash_t[BLAKE3_KEY_LEN];

  blake3_hasher _hasher;

  Hasher() { blake3_hasher_init(&_hasher); }

  void update(const void* data, size_t size) {
    blake3_hasher_update(&_hasher, data, size);
  }

  void finalize(hash_t hash) { blake3_hasher_finalize(&_hasher, hash, 0); }
};

#elif defined(BOOST_HASH)

#include <boost/hash2/sha3.hpp>

#elif !defined(LEAVES_CUSTOM_HASH)

typedef NullHasher Hasher;

#endif

#endif  // _LEAVES__HASH_HPP