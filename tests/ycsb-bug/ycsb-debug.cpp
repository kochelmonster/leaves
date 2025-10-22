#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <vector>

#include "../include/leaves/mmap.hpp"

using namespace leaves;

struct DebugEntry {
    std::string key;
    std::string base64_value;
    std::vector<uint8_t> decoded_value;
};

// Simple base64 decoder
std::vector<uint8_t> decode_base64(const std::string& encoded) {
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    
    std::string clean_input = encoded;
    // Remove any padding or whitespace
    while (!clean_input.empty() && clean_input.back() == '=') {
        clean_input.pop_back();
    }
    
    int in_len = clean_input.length();
    
    for (int i = 0; i < in_len; i += 4) {
        uint32_t triple = 0;
        int valid_chars = 0;
        
        // Process 4 characters at a time
        for (int j = 0; j < 4 && (i + j) < in_len; j++) {
            char c = clean_input[i + j];
            size_t pos = chars.find(c);
            
            if (pos != std::string::npos) {
                triple = (triple << 6) | pos;
                valid_chars++;
            }
        }
        
        // Pad the remaining bits if needed
        for (int j = valid_chars; j < 4; j++) {
            triple <<= 6;
        }
        
        // Extract bytes (we get 3 bytes from 4 base64 chars)
        if (valid_chars >= 2) result.push_back((triple >> 16) & 0xFF);
        if (valid_chars >= 3) result.push_back((triple >> 8) & 0xFF);  
        if (valid_chars >= 4) result.push_back(triple & 0xFF);
    }
    
    return result;
}

std::vector<DebugEntry> parse_debug_log(const std::string& log_path) {
    std::vector<DebugEntry> entries;
    std::ifstream file(log_path);
    
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open debug log file: " + log_path);
    }
    
    std::string line;
    std::regex pattern(R"(PUT #\d+ - Key: \[([^\]]+)\] Base64: \[([^\]]+)\])");
    
    while (std::getline(file, line)) {
        std::smatch matches;
        if (std::regex_search(line, matches, pattern)) {
            DebugEntry entry;
            entry.key = matches[1].str();
            entry.base64_value = matches[2].str();
            
            try {
                entry.decoded_value = decode_base64(entry.base64_value);
                entries.push_back(entry);
                std::cout << "Parsed key: " << entry.key 
                         << " (value: " << entry.decoded_value.size() << " bytes)" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error decoding base64 for key " << entry.key 
                         << ": " << e.what() << std::endl;
            }
        }
    }
    
    return entries;
}

void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " <ycsb.lvs_path> <debug.log_path>\n";
    std::cerr << "\n";
    std::cerr << "Debug tool for YCSB flaw reproduction.\n";
    std::cerr << "Loads ycsb.lvs database and inserts key-value pairs from debug log.\n";
    std::cerr << "\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  ycsb.lvs_path   Path to the YCSB database file\n";
    std::cerr << "  debug.log_path  Path to the leaves_debug.log file\n";
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string db_path = argv[1];
    std::string log_path = argv[2];
    
    try {
        // Parse the debug log
        std::cout << "Parsing debug log: " << log_path << std::endl;
        std::vector<DebugEntry> entries = parse_debug_log(log_path);
        std::cout << "Found " << entries.size() << " debug entries" << std::endl;
        
        if (entries.empty()) {
            std::cerr << "No valid entries found in debug log" << std::endl;
            return 1;
        }
        
        // Load the YCSB database
        std::cout << "\nLoading YCSB database: " << db_path << std::endl;
        MapStorage storage(db_path.c_str());
        auto db = storage["usertable"];
        
        std::cout << "Database loaded successfully" << std::endl;
        
        // Create cursor for database operations
        auto cursor = db.cursor();
        std::cout << "\nCreated database cursor" << std::endl;
        
        // Insert each debug entry
        size_t success_count = 0;
        size_t error_count = 0;
        const size_t COMMIT_BATCH_SIZE = 1000;
        
        for (size_t i = 0; i < entries.size(); i++) {
            const auto& entry = entries[i];
            
            try {
                // Insert the key-value pair
                Slice key_slice(entry.key.c_str(), entry.key.length());
                Slice value_slice(reinterpret_cast<const char*>(entry.decoded_value.data()), 
                                entry.decoded_value.size());
                
                std::cout << "Inserting #" << (i + 1) << " - Key: '" << entry.key 
                         << "' (value: " << entry.decoded_value.size() << " bytes)";
 
                if (i + 1 == 2053) {
                    std::cout << "  <-- Debug breakpoint here" << std::endl;
                }

                cursor.find(key_slice);
                cursor.value(value_slice);
                success_count++;
                std::cout << " ✓" << std::endl;
                
                // Commit every 1000 inserts
                if ((i + 1) % COMMIT_BATCH_SIZE == 0) {
                    std::cout << "Committing batch at " << (i + 1) << " inserts..." << std::endl;
                    cursor.commit();
                }
                
            } catch (const std::exception& e) {
                error_count++;
                std::cout << " ✗ Error: " << e.what() << std::endl;
            }
        }
        
        // Commit any remaining transactions
        if (entries.size() % COMMIT_BATCH_SIZE != 0) {
            std::cout << "\nCommitting final batch..." << std::endl;
            cursor.commit();
        }
        
        std::cout << "\n=== Summary ===" << std::endl;
        std::cout << "Total entries: " << entries.size() << std::endl;
        std::cout << "Successful inserts: " << success_count << std::endl;
        std::cout << "Failed inserts: " << error_count << std::endl;
        
        if (error_count > 0) {
            std::cout << "\n⚠️  Errors occurred during insertion. This might indicate the YCSB flaw." << std::endl;
            return 1;
        } else {
            std::cout << "\n✅ All entries inserted successfully." << std::endl;
        }
        
        // Verify some of the inserted data
        std::cout << "\n=== Verification ===" << std::endl;
        auto read_cursor = db.cursor();
        
        for (size_t i = 0; i < std::min(size_t(3), entries.size()); i++) {
            const auto& entry = entries[i];
            Slice key_slice(entry.key.c_str(), entry.key.length());
            
            try {
                read_cursor.find(key_slice);
                if (read_cursor.is_valid()) {
                    Slice retrieved_value = read_cursor.value();
                    std::cout << "✓ Key '" << entry.key << "' found with " 
                             << retrieved_value.size() << " bytes" << std::endl;
                } else {
                    std::cout << "✗ Key '" << entry.key << "' not found" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "✗ Error reading key '" << entry.key << "': " << e.what() << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}