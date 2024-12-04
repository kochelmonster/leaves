/*
g++ -o sorted -O3 sorted.cpp
*/

#include <stdint.h>
#include <chrono>
#include <iostream>
#include <cstring>
#include <algorithm> 

const int COUNT = 10000000;



char array[10];
int size = 0;


void insert(char* array, char character) {

    int i;
    for (i = size - 1; (i >= 0 && array[i] > character); i--) {
        array[i + 1] = array[i]; 
    }
    // Insert the new character
    array[i + 1] = character;
    size++;
}

bool find(char* array, char character) {
    auto compare = [](char a, char b) {
        return a < b;
    };
    return std::binary_search(array, array + size, character, compare);
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
