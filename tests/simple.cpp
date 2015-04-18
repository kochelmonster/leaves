#include <iostream>
#include <sstream>
#include "larch/leaves.h"

namespace larch_leaves {
  void decode(const std::string& input, std::string& output);
}

using namespace larch_leaves;

inline Slice inv(const std::string&  src) {
  static std::string buffer;
  decode(src, buffer);
  return Slice(buffer);
}

int main(int argc, const char* argv[]) {
  std::shared_ptr<MemoryDatabase> db = MemoryDatabase::create();
  std::shared_ptr<Cursor> cursor = db->writer();
  
  dumpb64(Slice("::main"), std::cout);

  std::string input((const char[]){ 0, 1, 2, 0 }, 4);
  
  dumpb64(inv(input), std::cout);
  
  cursor->set(inv(input));
  cursor->set_value(Slice("value-1"));
  db->dump(std::cout);
  
  input.append(2, 0);
  std::cout << "size: " << input.size() << std::endl;
  dumpb64(inv(input), std::cout);

  for(size_t i = 0; i < 64; i++) {
    std::stringstream f;
    f << "value-" << i;
    input.at(3) = i;
    cursor->set(inv(input));
    cursor->set_value(Slice(f.str()));
    db->dump(std::cout);
  }
}
