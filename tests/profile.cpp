#include <iostream>
#include <fstream>
#include <leaves.hpp>


#define TEST_FILE "test.lvs"

using namespace leaves;

const size_t COUNT = 10000;

#ifdef DEBUG
namespace leaves {
size_t dump_db(std::ostream& out, DB::db_ptr db);
}

inline void dump_graph(const char* output, leaves::DB::db_ptr db) {
  std::ofstream out(output);
  leaves::dump_db(out, db);
}
#endif



struct Preparation {
  Preparation() {
    std::remove(TEST_FILE);
  }

  ~Preparation() {
    std::remove(TEST_FILE);
    std::cout << "remove " << TEST_FILE << std::endl;
  }
};

int main(int argc, char** argv) {
    Preparation p;
    DB::db_ptr db(DB::open(TEST_FILE));
    DB::cursor_ptr cursor(db->create_cursor());
    
    std::string val = std::string(100, 1);
    leaves::Slice mkey, mval(val);
    
    for(size_t i = 0; i < COUNT; i++) {
        char key[100];
        snprintf(key, sizeof(key), "%016d", (int)i);
        mkey = leaves::Slice(key);
        cursor->find(mkey);
        val[0] = (char)i;
        cursor->set_value(mval);
        cursor->commit();
        /*if (i + 1 % 1000) {
            std::cout << "added " << i << std::endl;
        }*/
    }
    #ifdef DEBUG
    //dump_graph("debug.yaml", db);
    #endif

    return 0;
}