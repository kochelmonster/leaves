//@+leo-ver=5-thin
//@+node:michael.20141230111914.136: * @file base64.cpp
//@@language cplusplus
//@@tabwidth -2

//@+<< includes >>
//@+node:michael.20141230111914.137: ** << includes >>
#include <vector>
#include <boost/cstdint.hpp>
#include "conversion.hpp"
#include "larch/leaves.h"
#ifdef DEBUG
#include <iomanip>
#endif
//@-<< includes >>

namespace larch_leaves {

//@+others
//@+node:michael.20141230111914.138: ** encode
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
  size_t size = input.size();
  size_t output_size = 8*(size/6);
  size_t div_size = size % 6;
  
  switch(div_size) {
    case 1: output_size += 2; break;
    case 2: output_size += 3; break;
    case 3: output_size += 4; break;
    case 4: output_size += 6; break;
    case 5: output_size += 7; break;
  }

  output.resize(output_size);

  const char* s = input.data();
  boost::uint64_t* d = (boost::uint64_t*)&output[0];
  int max_i = (int)(size-8), i = 0, j = 0;
  for(; i <= max_i; i += 6, j += 8, s += 6, d++)
    *d = encode_block(*(boost::uint64_t*)s);

  max_i = (int)(size - i);
  boost::uint64_t block = 0;
  if (max_i >= 6) {
    memcpy(&block, s, 6);
    *d = encode_block(block);
    max_i -= 6;
    j += 8;
    s += 6;
    d++;
  }
  
  if (max_i > 0) {
    block = 0;
    memcpy(&block, s, max_i);
    block = encode_block(block);
    memcpy(d, &block, output_size-j);
  }
}
//@+node:michael.20141230111914.139: ** decode
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
  size_t size = input.size();
  size_t output_size = 6*(size/8);
  size_t div_size = size % 8;

  switch(div_size) {
    case 1: assert(0);
    case 2: output_size += 1; break;
    case 3: output_size += 2; break;
    case 4: output_size += 3; break;
    case 5: assert(0);
    case 6: output_size += 4; break;
    case 7: output_size += 5; break;
  }

  output.resize(output_size);

  boost::uint64_t* s = (boost::uint64_t*)&input[0];
  char* d = &output[0];
  int max_j = (int)(output_size - 8), i = 0, j = 0;
  
  for(; j < max_j; i += 8, j += 6, s++, d += 6)
    *(boost::uint64_t*)d = decode_block(*s);
  
  max_j = (int)(output_size - j);
  boost::uint64_t block = 0;
  if (max_j >= 6) {
    block = *s;
    block = decode_block(block);
    memcpy(d, &block, 6);
    max_j -= 6;
    i += 8;
    s++;
    d += 6;
  }
  
  if (max_j > 0) {
    block = 0;
    memcpy(&block, s, size-i);
    block = decode_block(block);
    memcpy(d, &block, max_j);
  }
}
//@+node:michael.20150101205559.9: ** dumpb64
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
//@-others
} // namespace larch_leaves 
//@-leo
