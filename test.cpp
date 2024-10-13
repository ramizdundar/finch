#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <iterator> // For std::next
#include <algorithm> // For std::remove

#define FINCH_CLIENT_NO_MAIN // Exclude main function from client.cpp
#include "client.cpp"

// Total number of operations per client
const int OPERATIONS_PER_CLIENT = 100000; // Adjust as needed

// Number of client threads
const int NUM_CLIENTS = 10; // Adjust as needed

std::atomic<int> successful_operations(0);
std::atomic<int> failed_operations(0);
std::atomic<int> total_operations_completed(0); // For progress tracking
std::mutex cout_mutex; // For synchronized console output

void client_thread_function(int client_id) {
    FinchClient client; // Each thread has its own client instance

    std::unordered_map<std::string, std::string> local_store; // Local map to track keys and values
    std::vector<std::string> keys; // Vector to store keys for random access
    std::unordered_set<std::string> key_set; // Set to ensure uniqueness in keys vector

    std::mt19937 rng(client_id); // Seed with client_id for variability
    std::uniform_int_distribution<int> op_dist(1, 100);
    std::uniform_int_distribution<int> key_length_dist(5, 15);
    std::uniform_int_distribution<int> value_length_dist(5, 50);
    std::uniform_int_distribution<int> char_dist('a', 'z');

    const int progress_interval = 10000; // Adjust as needed for more frequent updates

    for (int i = 0; i < OPERATIONS_PER_CLIENT; ++i) {
        int op_choice = op_dist(rng);

        try {
            if (op_choice <= 40) { // 40% chance to perform PUT
                // Generate random key and value
                int key_length = key_length_dist(rng);
                int value_length = value_length_dist(rng);

                // Build key with client ID prefix
                std::string key = std::to_string(client_id);
                for (int j = 0; j < key_length; ++j) {
                    key += static_cast<char>(char_dist(rng));
                }

                // Generate value
                std::string value(value_length, '\0');
                for (int j = 0; j < value_length; ++j) {
                    value[j] = static_cast<char>(char_dist(rng));
                }

                // Perform PUT operation
                if (client.put(key, value)) {
                    successful_operations++;
                    // If key is not already in key_set, add to keys vector
                    if (key_set.insert(key).second) {
                        keys.push_back(key); // Add key to vector
                    }
                    // Store key-value in local map
                    local_store[key] = value;
                } else {
                    failed_operations++;
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cerr << "Client " << client_id << " failed to PUT key: " << key << "\n";
                }
            } else if (op_choice <= 80) { // 40% chance to perform GET
                if (keys.empty()) continue; // No keys to get

                // Randomly select a key from keys vector
                std::string key = keys[rng() % keys.size()];

                // Perform GET operation
                std::string value = client.get(key);
                if (!value.empty()) {
                    if (value == local_store[key]) {
                        successful_operations++;
                    } else {
                        failed_operations++;
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        std::cerr << "Client " << client_id << " GET value mismatch for key: " << key << "\n";
                    }
                } else {
                    // Key not found
                    failed_operations++;
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cerr << "Client " << client_id << " failed to GET key: " << key << "\n";
                }
            } else { // 20% chance to perform DEL
                if (keys.empty()) continue; // No keys to delete

                // Randomly select a key from keys vector
                std::string key = keys[rng() % keys.size()];

                // Perform DEL operation
                if (client.del(key)) {
                    successful_operations++;
                    // Remove key from local map
                    local_store.erase(key);
                    // Remove key from key_set
                    key_set.erase(key);
                    // Remove all instances of key from keys vector
                    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
                } else {
                    failed_operations++;
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cerr << "Client " << client_id << " failed to DEL key: " << key << "\n";
                }
            }
        } catch (const std::exception& e) {
            failed_operations++;
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Client " << client_id << " exception: " << e.what() << "\n";
        }

        // Update progress
        if ((i + 1) % progress_interval == 0) {
            int completed = total_operations_completed.fetch_add(progress_interval) + progress_interval;
            int total_operations = NUM_CLIENTS * OPERATIONS_PER_CLIENT;
            double percentage = (double)completed / total_operations * 100.0;

            // Use cout_mutex to synchronize output
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Progress: " << completed << "/" << total_operations
                          << " operations completed (" << percentage << "%)\n";
            }
        }
    }

    // Handle any remaining operations not captured by the interval
    int remaining_ops = OPERATIONS_PER_CLIENT % progress_interval;
    if (remaining_ops > 0) {
        int completed = total_operations_completed.fetch_add(remaining_ops) + remaining_ops;
        int total_operations = NUM_CLIENTS * OPERATIONS_PER_CLIENT;
        double percentage = (double)completed / total_operations * 100.0;

        // Use cout_mutex to synchronize output
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "Progress: " << completed << "/" << total_operations
                      << " operations completed (" << percentage << "%)\n";
            }
    }

    // Final validation: Ensure all remaining keys return the expected values
    for (const auto& pair : local_store) {
        try {
            std::string value = client.get(pair.first);
            if (value == pair.second) {
                successful_operations++;
            } else {
                failed_operations++;
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cerr << "Client " << client_id << " final validation failed for key: " << pair.first << "\n";
            }
        } catch (const std::exception& e) {
            failed_operations++;
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cerr << "Client " << client_id << " exception during final validation: " << e.what() << "\n";
        }
    }
}

int main() {
    // Start the server before running this test
    std::cout << "Starting test with " << NUM_CLIENTS << " clients, each performing " << OPERATIONS_PER_CLIENT << " operations.\n";

    std::vector<std::thread> client_threads;

    // Launch client threads
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        client_threads.emplace_back(client_thread_function, i);
    }

    // Wait for all threads to complete
    for (auto& thread : client_threads) {
        thread.join();
    }

    // Output test results
    std::cout << "Test completed.\n";
    std::cout << "Successful operations: " << successful_operations.load() << "\n";
    std::cout << "Failed operations: " << failed_operations.load() << "\n";

    return 0;
}
