#include <cstdint>

struct ArrayNode {
  struct Item {
    uint8_t key;
    uint16_t child;
  };

  union {
    char buffer[32];
    struct {
      Item items[10];
      char size;
    };
  };

  void insert(char)

};