#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <cstring>

#include "../include/leaves/intern/_check.hpp"
#include "../include/leaves/mmap.hpp"
#include "../include/leaves/fstore.hpp"

using namespace leaves;

enum class StorageType {
    UNKNOWN,
    MMAP,
    FSTORE
};

StorageType detect_storage_type(const std::string& input_path) {
    std::ifstream file(input_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open input file: " + input_path);
    }
    
    // Read enough bytes to check both signatures
    constexpr size_t mmap_sig_len = sizeof(leaves::MMAP_SIGNATURE);
    constexpr size_t fstore_sig_len = sizeof(leaves::FSTORE_SIGNATURE);
    size_t max_sig_len = std::max(mmap_sig_len, fstore_sig_len);
    std::vector<char> buffer(max_sig_len, 0);
    file.read(buffer.data(), max_sig_len - 1);  // -1 to account for null terminator
    
    if (file.gcount() < static_cast<std::streamsize>(std::min(mmap_sig_len - 1, fstore_sig_len - 1))) {
        return StorageType::UNKNOWN;
    }
    
    // Check for MapStorage signature
    if (std::memcmp(buffer.data(), leaves::MMAP_SIGNATURE, mmap_sig_len - 1) == 0) {
        return StorageType::MMAP;
    }
    
    // Check for FileStorage signature  
    if (std::memcmp(buffer.data(), leaves::FSTORE_SIGNATURE, fstore_sig_len - 1) == 0) {
        return StorageType::FSTORE;
    }
    
    return StorageType::UNKNOWN;
}

void dump_mmap_storage(const std::string& input_path, const std::string& db_name, std::ostream& output) {
    try {
        MapStorage storage(input_path.c_str());
        auto db = storage[db_name.c_str()];
        
        _Dumper dumper(db, db._internal()->_wtxn->root, false);
        dumper.dump(output);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error dumping MapStorage: " + std::string(e.what()));
    }
}

void dump_fstore_storage(const std::string& input_path, const std::string& db_name, std::ostream& output) {
    try {
        FileStorage storage(input_path.c_str());
        auto db = storage[db_name.c_str()];
        
        _Dumper dumper(db, db._internal()->_wtxn->root, false);
        dumper.dump(output);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error dumping FileStorage: " + std::string(e.what()));
    }
}

void list_mmap_databases(const std::string& input_path) {
    try {
        MapStorage storage(input_path.c_str());
        std::vector<std::string> db_names;
        storage.list_dbs(db_names);
        
        std::cout << "Available databases in MapStorage file '" << input_path << "':" << std::endl;
        if (db_names.empty()) {
            std::cout << "  (no databases found)" << std::endl;
        } else {
            for (const auto& name : db_names) {
                if (!name.empty()) {
                    std::cout << "  " << name << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error listing MapStorage databases: " + std::string(e.what()));
    }
}

void list_fstore_databases(const std::string& input_path) {
    try {
        FileStorage storage(input_path.c_str());
        std::vector<std::string> db_names;
        storage.list_dbs(db_names);
        
        std::cout << "Available databases in FileStorage file '" << input_path << "':" << std::endl;
        if (db_names.empty()) {
            std::cout << "  (no databases found)" << std::endl;
        } else {
            for (const auto& name : db_names) {
                if (!name.empty()) {
                    std::cout << "  " << name << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error listing FileStorage databases: " + std::string(e.what()));
    }
}

void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " <input_path> [database] [output_path]\n";
    std::cerr << "\n";
    std::cerr << "Dumps a leaves database to YAML format.\n";
    std::cerr << "Automatically detects MapStorage or FileStorage based on file signature.\n";
    std::cerr << "\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  input_path   Path to the leaves database file\n";
    std::cerr << "  database     Name of the database to dump (optional)\n";
    std::cerr << "               If not specified, lists available databases\n";
    std::cerr << "  output_path  Path to output YAML file (optional, defaults to stdout)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string input_path = argv[1];
    std::string database_name;
    std::string output_path;
    bool use_stdout = true;
    bool list_databases = false;
    
    if (argc == 2) {
        // Only input path provided - list databases
        list_databases = true;
    } else if (argc == 3) {
        // input_path and database_name provided
        database_name = argv[2];
    } else if (argc == 4) {
        // input_path, database_name, and output_path provided
        database_name = argv[2];
        output_path = argv[3];
        use_stdout = false;
    }
    
    // Check if input file exists
    if (!std::filesystem::exists(input_path)) {
        std::cerr << "Error: Input file does not exist: " << input_path << std::endl;
        return 1;
    }
    
    try {
        // Detect storage type
        StorageType storage_type = detect_storage_type(input_path);
        
        if (storage_type == StorageType::UNKNOWN) {
            std::cerr << "Error: Unknown storage type. File does not have a valid leaves signature." << std::endl;
            return 1;
        }
        
        // If listing databases, do that and exit
        if (list_databases) {
            switch (storage_type) {
                case StorageType::MMAP:
                    list_mmap_databases(input_path);
                    break;
                    
                case StorageType::FSTORE:
                    list_fstore_databases(input_path);
                    break;
                    
                default:
                    std::cerr << "Error: Unsupported storage type" << std::endl;
                    return 1;
            }
            return 0;
        }
        
        // Set up output stream for dumping
        std::unique_ptr<std::ofstream> file_stream;
        std::ostream* output_stream = &std::cout;
        
        if (!use_stdout) {
            file_stream = std::make_unique<std::ofstream>(output_path);
            if (!file_stream->is_open()) {
                std::cerr << "Error: Cannot open output file: " << output_path << std::endl;
                return 1;
            }
            output_stream = file_stream.get();
        }
        
        // Dump based on detected storage type
        switch (storage_type) {
            case StorageType::MMAP:
                std::cerr << "Detected MapStorage format" << std::endl;
                dump_mmap_storage(input_path, database_name, *output_stream);
                break;
                
            case StorageType::FSTORE:
                std::cerr << "Detected FileStorage format" << std::endl; 
                dump_fstore_storage(input_path, database_name, *output_stream);
                break;
                
            default:
                std::cerr << "Error: Unsupported storage type" << std::endl;
                return 1;
        }
        
        if (!use_stdout) {
            std::cerr << "Successfully dumped to: " << output_path << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
