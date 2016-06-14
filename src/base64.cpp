#include <vector>
#include <boost/cstdint.hpp>
#include "conversion.hpp"
#include "../include/larch/leaves.hpp"
#ifdef DEBUG
#include <iomanip>
#endif

namespace larch_leaves {

inline boost::uint64_t encode_block(boost::uint64_t block) {
  boost::endian::big_to_native(block);
  block = block >> 2;
  block = (block & 0xFF00000000000000) | ((block & 0x00FFFFFFFFFFFFFF) >> 2);
  block = (block & 0xFFFF000000000000) | ((block & 0x0000FFFFFFFFFFFF) >> 2);
  block = (block & 0xFFFFFF0000000000) | ((block & 0x000000FFFFFFFFFF) >> 2);
  block = (block & 0xFFFFFFFF00000000) | ((block & 0x00000000FFFFFFFF) >> 2);
  block = (block & 0xFFFFFFFFFF000000) | ((block & 0x0000000000FFFFFF) >> 2);
  block = (block & 0xFFFFFFFFFFFF0000) | ((block & 0x000000000000FFFF) >> 2);
  block = (block & 0xFFFFFFFFFFFFFF00) | ((block & 0x00000000000000FF) >> 2);
  boost::endian::native_to_big(block);
  return block;
}

void encode(const Slice& input, std::string& output) {
  size_t main_size, pad_size = 0;

  main_size = 8*(input.size()/6);
  switch(input.size()%6) {
  case 1: pad_size = 2; break;
  case 2: pad_size = 3; break;
  case 3: pad_size = 4; break;
  case 4: pad_size = 6; break;
  case 5: pad_size = 7; break;
  }

  output.resize(main_size+pad_size);
  size_t i, j;
  for(i = j = 0; i < input.size(); i += 6, j+= 8) {
    *(boost::uint64_t*)&output[j] = encode_block(*(boost::uint64_t*)
						 (input.data()+i));
  }
  if (pad_size) {
    i -= 6;
    j -= 8;
    boost::uint64_t block = 0;
    memcpy(&block, input.data()+i, input.size()-i);
    block = encode_block(block);
    memcpy((void*)&output[j], &block, pad_size);
  }
}
inline boost::uint64_t decode_block(boost::uint64_t block) {
  boost::endian::big_to_native(block);
  block = (block & 0xFFFFFFFFFFFFFF00) | ((block & 0x00000000000000FF) << 2);
  block = (block & 0xFFFFFFFFFFFF0000) | ((block & 0x000000000000FFFF) << 2);
  block = (block & 0xFFFFFFFFFF000000) | ((block & 0x0000000000FFFFFF) << 2);
  block = (block & 0xFFFFFFFF00000000) | ((block & 0x00000000FFFFFFFF) << 2);
  block = (block & 0xFFFFFF0000000000) | ((block & 0x000000FFFFFFFFFF) << 2);
  block = (block & 0xFFFF000000000000) | ((block & 0x0000FFFFFFFFFFFF) << 2);
  block = (block & 0xFF00000000000000) | ((block & 0x00FFFFFFFFFFFFFF) << 2);
  block = block << 2;
  boost::endian::native_to_big(block);
  return block;
}

  void decode(const std::string& input, std::string& output) {
  size_t main_size, pad_size = 0;

  main_size = 6*(input.size()/8);
  switch(input.size()%8) {
  case 2: pad_size = 1; break;
  case 3: pad_size = 2; break;
  case 4: pad_size = 3; break;
  case 6: pad_size = 4; break;
  case 7: pad_size = 5; break;
  }

  output.resize(main_size+pad_size);
  size_t i, j;
  boost::uint64_t block;
  for(i = j = 0; i < input.size(); i += 8, j+= 6) {
    block = decode_block(*(boost::uint64_t*)(input.data()+i));
    memcpy((void*)&output[j], &block, 6);
  }
  if (pad_size) {
    i -= 8;
    j -= 6;
    block = 0;
    memcpy(&block, input.data()+i, input.size()-i);
    block = decode_block(block);
    memcpy((void*)&output[j], &block, pad_size);
  }
}
#ifdef DEBUG
void dumpb64(const Slice& key, std::ostream& out) {
    std::string encoded;
    encode(key, encoded);
    out << "input: " << key.string() << "(" << key.size() << ")" << std::endl
        << "output: "
        << std::setw(2) << std::setfill('0')
        << (int)encoded[0];

    for(size_t i = 1; i < encoded.size(); i++)
        out << "|"
            << std::setw(2) << std::setfill('0')
            << (int)encoded[i];

    out << std::endl;
  }
#endif
} // namespace larch_leaves
