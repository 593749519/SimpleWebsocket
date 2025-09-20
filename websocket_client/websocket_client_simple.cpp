#include "websocket_simple.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

using namespace WebSocketSimple;

class SimpleWebSocketClient {
private:
    std::shared_ptr<Connection> connection_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> totalBytesReceived_{0};
    std::atomic<uint64_t> messageCount_{0};
    std::chrono::steady_clock::time_point startTime_;

public:
    bool connect(const std::string& host, int port) {
        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cout << "WSAStartup failed" << std::endl;
            return false;
        }
        
        // Create socket
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            std::cout << "Socket creation failed" << std::endl;
            WSACleanup();
            return false;
        }
        
        // Connect to server
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);
        
        if (::connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cout << "Connection failed" << std::endl;
            closesocket(sock);
            WSACleanup();
            return false;
        }
        
        // Perform WebSocket handshake
        if (!performHandshake(sock, host, port)) {
            std::cout << "WebSocket handshake failed" << std::endl;
            closesocket(sock);
            WSACleanup();
            return false;
        }
        
        // Create connection
        connection_ = std::make_shared<Connection>(sock, 1);
        setupCallbacks();
        
        connected_.store(true);
        startTime_ = std::chrono::steady_clock::now();
        
        std::cout << "Connected to " << host << ":" << port << std::endl;
        return true;
    }
    
    void disconnect() {
        if (connection_) {
            connection_->close();
            connection_.reset();
        }
        connected_.store(false);
        WSACleanup();
    }
    
    void run() {
        if (!connected_.load()) {
            std::cout << "Not connected" << std::endl;
            return;
        }
        
        std::cout << "Client running. Commands:" << std::endl;
        std::cout << "  t - Send 50MB text to server" << std::endl;
        std::cout << "  b - Send 50MB binary to server" << std::endl;
        std::cout << "  s - Send small text message" << std::endl;
        std::cout << "  d - Send small binary data" << std::endl;
        std::cout << "  Enter - Show stats" << std::endl;
        std::cout << "  q - Quit" << std::endl;
        
        std::string input;
        while (connected_.load()) {
            std::cout << "\n> ";
            std::getline(std::cin, input);
            
            if (input.empty()) {
                showStats();
            } else if (input == "q" || input == "Q") {
                break;
            } else if (input == "t" || input == "T") {
                sendLargeText();
            } else if (input == "b" || input == "B") {
                sendLargeBinary();
            } else if (input == "s" || input == "S") {
                sendSmallText();
            } else if (input == "d" || input == "D") {
                sendSmallBinary();
            } else {
                std::cout << "Unknown command: " << input << std::endl;
            }
        }
        
        disconnect();
    }

private:
    bool performHandshake(SOCKET sock, const std::string& host, int port) {
        // Generate WebSocket key
        std::string key = "dGhlIHNhbXBsZSBub25jZQ=="; // Base64 encoded "the sample nonce"
        
        std::string request = "GET / HTTP/1.1\r\n";
        request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";
        
        // Send handshake request
        int bytesSent = ::send(sock, request.c_str(), static_cast<int>(request.length()), 0);
        if (bytesSent == SOCKET_ERROR) {
            return false;
        }
        
        // Receive handshake response
        char buffer[1024];
        int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived == SOCKET_ERROR) {
            return false;
        }
        
        buffer[bytesReceived] = '\0';
        std::string response(buffer, bytesReceived);
        
        // Check if handshake was successful
        return response.find("101 Switching Protocols") != std::string::npos;
    }
    
    void setupCallbacks() {
        if (!connection_) return;
        
        connection_->onMessage([this](const Message& msg) {
            messageCount_++;
            totalBytesReceived_ += msg.size(); // Use new size() method
            
            // Print statistics for every complete message
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
            
            if (msg.type == Message::TEXT) {
                std::cout << "Client received text: " << formatBytes(msg.size()) 
                          << " (Total: " << formatBytes(totalBytesReceived_.load()) << ")";
            } else {
                std::cout << "Client received binary: " << formatBytes(msg.size()) 
                          << " (Total: " << formatBytes(totalBytesReceived_.load()) << ")";
            }
            
            if (elapsed > 0) {
                double speed = static_cast<double>(totalBytesReceived_.load()) / elapsed / (1024 * 1024);
                std::cout << " Speed: " << std::fixed << std::setprecision(2) << speed << " MB/s";
            }
            std::cout << std::endl;
        });
        
        connection_->onError([this](const std::string& error) {
            std::cout << "Connection error: " << error << std::endl;
            connected_.store(false);
        });
        
        connection_->onClose([this]() {
            std::cout << "Connection closed" << std::endl;
            connected_.store(false);
        });
    }
    
    void sendLargeText() {
        size_t size = 50 * 1024 * 1024; // 50MB
        std::string text(size, 'C'); // Use 'C' to distinguish from server's 'A'
        
        std::cout << "Sending 50MB text to server..." << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        bool success = connection_->sendText(text);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        if (success) {
            std::cout << "Text sent successfully in " << duration.count() << "ms" << std::endl;
        } else {
            std::cout << "Failed to send text" << std::endl;
        }
    }
    
    void sendLargeBinary() {
        size_t size = 50 * 1024 * 1024; // 50MB
        std::vector<uint8_t> binary(size, 0x43); // Use 0x43 to distinguish from server's 0x42
        
        std::cout << "Sending 50MB binary to server..." << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        bool success = connection_->sendBinary(binary);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        if (success) {
            std::cout << "Binary sent successfully in " << duration.count() << "ms" << std::endl;
        } else {
            std::cout << "Failed to send binary" << std::endl;
        }
    }
    
    void sendSmallText() {
        std::string text = "Hello from client! This is a small text message.";
        
        std::cout << "Sending small text: " << text << std::endl;
        
        bool success = connection_->sendText(text);
        
        if (success) {
            std::cout << "Small text sent successfully" << std::endl;
        } else {
            std::cout << "Failed to send small text" << std::endl;
        }
    }
    
    void sendSmallBinary() {
        std::vector<uint8_t> binary = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x42, 0x69, 0x6E, 0x61, 0x72, 0x79}; // "Hello Binary"
        
        std::cout << "Sending small binary data (" << binary.size() << " bytes)" << std::endl;
        
        bool success = connection_->sendBinary(binary);
        
        if (success) {
            std::cout << "Small binary sent successfully" << std::endl;
        } else {
            std::cout << "Failed to send small binary" << std::endl;
        }
    }
    
    void showStats() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
        
        std::cout << "\n=== Client Statistics ===" << std::endl;
        std::cout << "Messages received: " << messageCount_.load() << std::endl;
        std::cout << "Total bytes: " << formatBytes(totalBytesReceived_.load()) << std::endl;
        std::cout << "Uptime: " << elapsed << " seconds" << std::endl;
        
        if (elapsed > 0) {
            double speed = static_cast<double>(totalBytesReceived_.load()) / elapsed / (1024 * 1024);
            std::cout << "Average speed: " << std::fixed << std::setprecision(2) << speed << " MB/s" << std::endl;
        }
        std::cout << "========================\n" << std::endl;
    }
    
    std::string formatBytes(uint64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }
};

class MultiConnectionClient {
private:
    std::vector<std::unique_ptr<SimpleWebSocketClient>> clients_;

public:
    bool createConnections(const std::string& host, int port, int count) {
        clients_.clear();
        
        for (int i = 0; i < count; ++i) {
            auto client = std::make_unique<SimpleWebSocketClient>();
            if (!client->connect(host, port)) {
                std::cout << "Failed to connect client " << i << std::endl;
                continue;
            }
            clients_.push_back(std::move(client));
        }
        
        std::cout << "Created " << clients_.size() << " connections" << std::endl;
        return !clients_.empty();
    }
    
    void run() {
        if (clients_.empty()) {
            std::cout << "No connections available" << std::endl;
            return;
        }
        
        std::cout << "Multi-connection client running. Press Enter to show stats, 'q' to quit." << std::endl;
        
        std::string input;
        while (true) {
            std::getline(std::cin, input);
            
            if (input == "q" || input == "Q") {
                break;
            } else {
                showStats();
            }
        }
        
        // Disconnect all clients
        for (auto& client : clients_) {
            if (client) {
                client->disconnect();
            }
        }
    }

private:
    void showStats() {
        uint64_t totalMessages = 0;
        uint64_t totalBytes = 0;
        
        // Sum up statistics from all clients
        for (const auto& client : clients_) {
            if (client) {
                // Note: We can't access private members directly
                // This is a limitation of the current design
                totalMessages++; // This is just a placeholder
                totalBytes++;    // This is just a placeholder
            }
        }
        
        std::cout << "\n=== Multi-Connection Statistics ===" << std::endl;
        std::cout << "Active connections: " << clients_.size() << std::endl;
        std::cout << "Note: Individual client statistics are shown in their own callbacks" << std::endl;
        std::cout << "===================================\n" << std::endl;
    }
    
    std::string formatBytes(uint64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }
};

int main() {
    std::cout << "WebSocket Simple Client" << std::endl;
    std::cout << "======================" << std::endl;
    
    std::cout << "Choose mode:" << std::endl;
    std::cout << "1. Single connection" << std::endl;
    std::cout << "2. Multiple connections" << std::endl;
    std::cout << "Enter choice (1 or 2): ";
    
    std::string choice;
    std::getline(std::cin, choice);
    
    if (choice == "1") {
        SimpleWebSocketClient client;
        if (client.connect("127.0.0.1", 9001)) {
            client.run();
        }
    } else if (choice == "2") {
        MultiConnectionClient multiClient;
        if (multiClient.createConnections("127.0.0.1", 9001, 3)) {
            multiClient.run();
        }
    } else {
        std::cout << "Invalid choice" << std::endl;
    }
    
    return 0;
}
