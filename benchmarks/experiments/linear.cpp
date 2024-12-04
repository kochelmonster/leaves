/*
g++ -o linear -O3 linear.cpp
*/

#include <stdint.h>
#include <chrono>
#include <iostream>
#include <cstring>

const int COUNT = 10000000;


struct Item {
    char key;
    uint16_t child;
};

Item array[10];
int size = 0;


void insert(Item* array, char character) {
    array[size++].key = character;
}

bool find(Item* array, char character) {
    for(int i = 0; i < size; i++) {
        if(array[i].key == character) {
            return true;
        }
    }
    return false;
}


int main() {
    // Declare the array with the specified size

    const char* chars = "ABCDEFGHIJ";

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < COUNT; i++) {
        memset(array, 0, sizeof(array));
        size = 0;
        for(const char *p = chars; *p; p++) {
            insert(array, *p);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    std::cout << "Time taken for insert: " << duration.count() << " ms" << std::endl;


    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < COUNT; i++) {
        find(array, 'E');
    }

    end = std::chrono::high_resolution_clock::now();
    duration = end - start;
    std::cout << "Time taken for find: " << duration.count() << " ms" << std::endl;

    return 0;
    return 0;
}
