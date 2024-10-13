#include <iostream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <algorithm>

const int PARTITION_COUNT = 1024;
const int MAX_BUFFER_SIZE = 4096; // Increased buffer size

struct Partition {
    std::unordered_map<std::string, std::string> data;
    std::mutex mtx;
};

std::vector<Partition> partitions(PARTITION_COUNT);

struct ClientBuffer {
    std::vector<uint8_t> buffer;
};

uint32_t ntoh_uint32(uint32_t value) {
    return ntohl(value);
}

uint64_t ntoh_uint64(uint64_t value) {
    uint32_t high_part = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF));
    uint32_t low_part = ntohl(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(high_part) << 32) | low_part;
}

void handle_client(int client_sock) {
    ClientBuffer client_buffer;

    while (true) {
        uint8_t temp_buffer[MAX_BUFFER_SIZE];
        ssize_t bytes_received = recv(client_sock, temp_buffer, MAX_BUFFER_SIZE, 0);
        if (bytes_received <= 0) break;

        // Append received data to the client's buffer
        client_buffer.buffer.insert(client_buffer.buffer.end(), temp_buffer, temp_buffer + bytes_received);

        // Process messages in the buffer
        while (true) {
            if (client_buffer.buffer.size() < sizeof(uint32_t)) {
                // Not enough data to read total size
                break;
            }

            // Read Total Size (N)
            uint32_t total_size_net;
            std::memcpy(&total_size_net, &client_buffer.buffer[0], sizeof(uint32_t));
            uint32_t total_size = ntoh_uint32(total_size_net);

            if (client_buffer.buffer.size() < total_size) {
                // Wait for more data
                break;
            }

            // Extract the complete message
            std::vector<uint8_t> message(client_buffer.buffer.begin(), client_buffer.buffer.begin() + total_size);

            // Remove the message from the buffer
            client_buffer.buffer.erase(client_buffer.buffer.begin(), client_buffer.buffer.begin() + total_size);

            // Process the message
            size_t offset = 0;

            // Total Size (already read)
            offset += sizeof(uint32_t);

            // Operation Type
            uint8_t operation_type = message[offset];
            offset += sizeof(uint8_t);

            // Key Hash (not used here, but we can validate if needed)
            uint64_t key_hash_net;
            std::memcpy(&key_hash_net, &message[offset], sizeof(uint64_t));
            uint64_t key_hash = ntoh_uint64(key_hash_net);
            offset += sizeof(uint64_t);

            // Key Length
            uint32_t key_length_net;
            std::memcpy(&key_length_net, &message[offset], sizeof(uint32_t));
            uint32_t key_length = ntoh_uint32(key_length_net);
            offset += sizeof(uint32_t);

            if (offset + key_length > message.size()) {
                // Invalid message, key length exceeds message size
                send(client_sock, "1ERROR: Invalid message", 23, 0);
                continue;
            }

            // Key
            const char* key_ptr = reinterpret_cast<const char*>(&message[offset]);
            std::string key(key_ptr, key_ptr + key_length); // Fixed ambiguity
            offset += key_length;

            size_t hash = key_hash; // Use the key hash from the message
            int partition_id = hash % PARTITION_COUNT;

            if (operation_type == 1) { // GET
                std::string result;
                {
                    std::scoped_lock lock(partitions[partition_id].mtx);
                    auto it = partitions[partition_id].data.find(key);
                    if (it != partitions[partition_id].data.end()) {
                        result = "0" + it->second; // Prepend '0' for success
                    } else {
                        result = "1NOT_FOUND"; // Prepend '1' for error
                    }
                }
                send(client_sock, result.c_str(), result.size(), 0);
            } else if (operation_type == 2) { // PUT
                // Value Length
                if (offset + sizeof(uint32_t) > message.size()) {
                    send(client_sock, "1ERROR: Invalid message", 23, 0);
                    continue;
                }
                uint32_t value_length_net;
                std::memcpy(&value_length_net, &message[offset], sizeof(uint32_t));
                uint32_t value_length = ntoh_uint32(value_length_net);
                offset += sizeof(uint32_t);

                if (offset + value_length > message.size()) {
                    send(client_sock, "1ERROR: Invalid message", 23, 0);
                    continue;
                }

                // Value
                const char* value_ptr = reinterpret_cast<const char*>(&message[offset]);
                std::string value(value_ptr, value_ptr + value_length); // Fixed ambiguity
                offset += value_length;

                {
                    std::scoped_lock lock(partitions[partition_id].mtx);
                    partitions[partition_id].data[key] = value;
                }
                send(client_sock, "0OK", 3, 0); // Prepend '0' for success
            } else if (operation_type == 3) { // DEL
                bool erased = false;
                {
                    std::scoped_lock lock(partitions[partition_id].mtx);
                    erased = partitions[partition_id].data.erase(key) > 0;
                }

                std::string response = erased ? "0DELETED" : "1NOT_FOUND"; // Prepend '0' or '1'
                send(client_sock, response.c_str(), response.size(), 0);
            } else {
                send(client_sock, "1ERROR: Unknown command", 23, 0);
            }
        }
    }
    close(client_sock);
}

int main() {
    int server_sock;
    int port = 12345;
    while (true) {
        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock == -1) {
            std::cerr << "Failed to create socket.\n";
            return 1;
        }

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
            break;
        } else {
            close(server_sock);
            port++;  // Try next port
        }
    }

    std::cout << "Server listening on port " << port << "\n";

    if (listen(server_sock, SOMAXCONN) == -1) {
        std::cerr << "Failed to listen.\n";
        return 1;
    }

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_size = sizeof(client_addr);
        int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_size);
        if (client_sock == -1) {
            std::cerr << "Failed to accept client.\n";
            continue;
        }
        std::thread(handle_client, client_sock).detach();
    }

    close(server_sock);
    return 0;
}
