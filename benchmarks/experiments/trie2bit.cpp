/*
g++ -o trie2bit -O3 trie2bit.cpp
*/

#include <chrono>
#include <iostream>
#include <cstring>

const int COUNT = 10000000;

#define MAX_CHILDREN 4 // For 2-bit representation (00, 01, 10, 11)


struct TrieNode {
    struct TrieNode* children[MAX_CHILDREN];
    int isEndOfChar;

    static TrieNode* create();
};


TrieNode supply[512];
int count = 0;


TrieNode* TrieNode::create() {
    TrieNode* result = &supply[count++];
    memset(result, 0, sizeof(TrieNode));
    return result;
}


// Function to insert a character with its 2-bit representation
void insert(TrieNode* root, char character) {
    TrieNode* node = root;
    for (int i = 6; i >= 0; i -= 2) {
        int index = (character >> i) & 3;
        if (node->children[index] == NULL) {
            node->children[index] = TrieNode::create();
        }
        node = node->children[index];
    }
    node->isEndOfChar = 1; // Mark the end of the character
}

bool find(TrieNode* root, char character) {
    TrieNode* node = root;
    for (int i = 6; i >= 0; i -= 2) {
        int index = (character >> i) & 3;
        if (node->children[index] == NULL) {
            return false; // Character not found
        }
        node = node->children[index];
    }
    return true;
}



int main() {
    const char* chars = "ABCDEFGHIJ";

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < COUNT; i++) {
        TrieNode* root = TrieNode::create();
        for(const char *p = chars; *p; p++) {
            insert(root, *p);
        }
        count = 0;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    std::cout << "Time taken for insert: " << duration.count() << " ms" << std::endl;


    TrieNode* root = TrieNode::create();
    for(const char *p = chars; *p; p++) {
        insert(root, *p);
    }

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < COUNT; i++) {
        find(root, 'J');
    }

    end = std::chrono::high_resolution_clock::now();
    duration = end - start;
    std::cout << "Time taken for find: " << duration.count() << " ms" << std::endl;

    return 0;
}

