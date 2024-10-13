#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <stdexcept>
#include <unordered_map>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>

const int MAX_BUFFER_SIZE = 1024;

// Define operation types
const uint8_t OP_GET = 1;
const uint8_t OP_PUT = 2;
const uint8_t OP_DEL = 3;

struct ServerInfo {
    std::string address;
    int port;
};

class FinchClient {
public:
    FinchClient(const std::string& server_list_filename = "node_list.txt") {
        servers = read_server_list(server_list_filename);
        if (servers.empty()) {
            throw std::runtime_error("No servers found in node_list.txt");
        }

        // Initialize connections map
        for (size_t i = 0; i < servers.size(); ++i) {
            connections[i] = -1; // -1 indicates no connection
        }
    }

    ~FinchClient() {
        // Close all open sockets
        for (auto& conn : connections) {
            if (conn.second != -1) {
                close(conn.second);
            }
        }
    }

    std::string get(const std::string& key) {
        std::string response;
        char status_code;
        if (send_command(OP_GET, key, "", status_code, response)) {
            if (status_code == '0') {
                // Success, response contains the value
                return response;
            } else {
                // Failure, response contains error message
                return ""; // Key not found or error
            }
        } else {
            throw std::runtime_error("Failed to get the key: " + key);
        }
    }

    bool put(const std::string& key, const std::string& value) {
        std::string response;
        char status_code;
        if (send_command(OP_PUT, key, value, status_code, response)) {
            return status_code == '0';
        } else {
            return false;
        }
    }

    bool del(const std::string& key) {
        std::string response;
        char status_code;
        if (send_command(OP_DEL, key, "", status_code, response)) {
            if (status_code != '0') {
                // Failure, response contains error message
                std::cerr << "Failed to delete the key: " << key << " response: " << response << "\n";
            }
            return status_code == '0';
        } else {
            return false;
        }
    }

private:
    std::vector<ServerInfo> servers;
    std::unordered_map<size_t, int> connections; // Map from server ID to socket FD

    std::vector<ServerInfo> read_server_list(const std::string& filename) {
        std::vector<ServerInfo> servers;
        std::ifstream infile(filename);
        std::string line;
        while (std::getline(infile, line)) {
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                servers.push_back({line.substr(0, colon_pos), std::stoi(line.substr(colon_pos + 1))});
            }
        }
        return servers;
    }

    int connect_to_server(size_t server_id) {
        // Check if we already have a connection
        if (connections[server_id] != -1) {
            // Test if the connection is still alive
            if (is_socket_alive(connections[server_id])) {
                return connections[server_id];
            } else {
                // Connection is dead, close it
                close(connections[server_id]);
                connections[server_id] = -1;
            }
        }

        // Establish a new connection
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) return -1;

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(servers[server_id].port);
        inet_pton(AF_INET, servers[server_id].address.c_str(), &server_addr.sin_addr);

        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            close(sock);
            return -1;
        }

        connections[server_id] = sock;
        return sock;
    }

    bool is_socket_alive(int sock) {
        // Check if the socket is still connected
        char buffer;
        int result = recv(sock, &buffer, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) {
            // Connection has been closed
            return false;
        } else if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available, but socket is still alive
                return true;
            } else {
                // An error occurred
                return false;
            }
        } else {
            // Data is available to read
            return true;
        }
    }

    uint32_t hton_uint32(uint32_t value) {
        return htonl(value);
    }

    uint64_t hton_uint64(uint64_t value) {
        uint32_t high_part = htonl(static_cast<uint32_t>(value >> 32));
        uint32_t low_part = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));
        return (static_cast<uint64_t>(low_part) << 32) | high_part;
    }

    bool send_command(uint8_t op_type, const std::string& key, const std::string& value, char& status_code, std::string& response) {
        if (key.empty()) {
            std::cerr << "Key cannot be empty.\n";
            return false;
        }

        // Hash the key to determine the server
        std::hash<std::string> hasher;
        uint64_t key_hash = hasher(key);
        size_t server_id = key_hash % servers.size();

        int sock = connect_to_server(server_id);
        if (sock == -1) {
            std::cerr << "Failed to connect to server " << server_id << "\n";
            return false;
        }

        // Serialize the message according to the message structure
        std::vector<uint8_t> message;

        // Operation Type
        uint8_t operation_type = op_type;

        // Key Hash (uint64_t)
        uint64_t key_hash_net = hton_uint64(key_hash);

        // Key Length (uint32_t)
        uint32_t key_length = key.size();
        uint32_t key_length_net = hton_uint32(key_length);

        // Total Size (uint32_t)
        uint32_t total_size = sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) + key_length;
        if (operation_type == OP_PUT) { // PUT operation includes value
            total_size += sizeof(uint32_t) + value.size(); // Add Value Length and Value size
        }
        uint32_t total_size_net = hton_uint32(total_size);

        // Build the message
        // Append Total Size
        message.insert(message.end(), reinterpret_cast<uint8_t*>(&total_size_net), reinterpret_cast<uint8_t*>(&total_size_net) + sizeof(uint32_t));

        // Append Operation Type
        message.push_back(operation_type);

        // Append Key Hash
        message.insert(message.end(), reinterpret_cast<uint8_t*>(&key_hash_net), reinterpret_cast<uint8_t*>(&key_hash_net) + sizeof(uint64_t));

        // Append Key Length
        message.insert(message.end(), reinterpret_cast<uint8_t*>(&key_length_net), reinterpret_cast<uint8_t*>(&key_length_net) + sizeof(uint32_t));

        // Append Key
        message.insert(message.end(), key.begin(), key.end());

        if (operation_type == OP_PUT) { // PUT operation
            // Value Length (uint32_t)
            uint32_t value_length = value.size();
            uint32_t value_length_net = hton_uint32(value_length);

            // Append Value Length
            message.insert(message.end(), reinterpret_cast<uint8_t*>(&value_length_net), reinterpret_cast<uint8_t*>(&value_length_net) + sizeof(uint32_t));

            // Append Value
            message.insert(message.end(), value.begin(), value.end());
        }

        // Send the message
        size_t total_sent = 0;
        size_t message_size = message.size();
        while (total_sent < message_size) {
            ssize_t bytes_sent = send(sock, &message[total_sent], message_size - total_sent, 0);
            if (bytes_sent <= 0) {
                // Error occurred, try to reconnect
                close(sock);
                connections[server_id] = -1;
                sock = connect_to_server(server_id);
                if (sock == -1) {
                    std::cerr << "Failed to reconnect to server " << server_id << "\n";
                    return false;
                }
                // Retry sending the remaining data
                continue;
            }
            total_sent += bytes_sent;
        }

        // Receive the response
        char buffer[MAX_BUFFER_SIZE];
        memset(buffer, 0, MAX_BUFFER_SIZE);
        ssize_t bytes_received = recv(sock, buffer, MAX_BUFFER_SIZE - 1, 0);
        if (bytes_received > 0) {
            response.assign(buffer, bytes_received);
            // Remove any trailing newline characters
            response.erase(response.find_last_not_of("\n\r") + 1);
            if (response.empty()) {
                std::cerr << "Empty response from server " << server_id << "\n";
                return false;
            }
            status_code = response[0];
            response = response.substr(1); // Remove the status code
            return true;
        } else if (bytes_received == 0) {
            // Connection closed by server
            close(sock);
            connections[server_id] = -1;
            std::cerr << "Connection closed by server " << server_id << "\n";
            return false;
        } else {
            // Error occurred
            close(sock);
            connections[server_id] = -1;
            std::cerr << "Error receiving response from server " << server_id << "\n";
            return false;
        }
    }
};

// The main function is included only when compiling client.cpp directly
#ifndef FINCH_CLIENT_NO_MAIN

int main() {
    try {
        FinchClient client;

        // Example usage
        if (client.put("mykey", "myvalue")) {
            std::cout << "Key stored successfully.\n";
        } else {
            std::cout << "Failed to store the key.\n";
        }

        std::string value = client.get("mykey");
        if (!value.empty()) {
            std::cout << "Value retrieved: " << value << "\n";
        } else {
            std::cout << "Key not found.\n";
        }

        if (client.del("mykey")) {
            std::cout << "Key deleted successfully.\n";
        } else {
            std::cout << "Failed to delete the key.\n";
        }

        value = client.get("mykey");
        if (!value.empty()) {
            std::cout << "Value retrieved: " << value << "\n";
        } else {
            std::cout << "Key not found after deletion.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}

#endif // FINCH_CLIENT_NO_MAIN