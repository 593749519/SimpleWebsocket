#include "websocket_simple.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdio>

using namespace WebSocketSimple;

class SimpleWebSocketServer {
private:
    Server server_;
    std::atomic<bool> running_{false};
    std::thread commandThread_;

public:
    void start() {
        // Set up server callbacks
        server_.onConnection([this](std::shared_ptr<Connection> conn) {
            std::cout << "New connection: " << conn->getId() << std::endl;
            
            // Set up connection callbacks
            conn->onMessage([this, conn](const Message& msg) {
                if (msg.type == Message::TEXT) {
                    std::cout << "Server received text from connection " << conn->getId() 
                              << ": " << formatBytes(msg.size()) << std::endl;
                    
                    // Show content for small messages
                    if (msg.size() < 1000) {
                        std::cout << "  Content: " << msg.getTextData() << std::endl;
                    }
                } else {
                    std::cout << "Server received binary from connection " << conn->getId() 
                              << ": " << formatBytes(msg.size()) << std::endl;
                    
                    // Show content for small binary messages
                    if (msg.size() < 100) {
                        std::cout << "  Content: ";
                        const auto& binaryData = msg.getBinaryData();
                        for (size_t i = 0; i < binaryData.size() && i < 20; ++i) {
                            printf("0x%02X ", binaryData[i]);
                        }
                        if (binaryData.size() > 20) {
                            std::cout << "...";
                        }
                        std::cout << std::endl;
                    }
                }
            });
            
            conn->onError([conn](const std::string& error) {
                std::cout << "Connection " << conn->getId() << " error: " << error << std::endl;
            });
            
            conn->onClose([conn]() {
                std::cout << "Connection " << conn->getId() << " closed" << std::endl;
            });
        });
        
        server_.onError([](const std::string& error) {
            std::cout << "Server error: " << error << std::endl;
        });
        
        // Start server
        if (!server_.start(9001)) {
            std::cout << "Failed to start server on port 9001" << std::endl;
            return;
        }
        
        std::cout << "WebSocket Server started on port 9001" << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  t - Send 50MB text to all clients" << std::endl;
        std::cout << "  b - Send 50MB binary to all clients" << std::endl;
        std::cout << "  q - Quit" << std::endl;
        std::cout << "  Enter - Show status" << std::endl;
        
        running_.store(true);
        commandThread_ = std::thread(&SimpleWebSocketServer::commandLoop, this);
        
        // Wait for command thread
        if (commandThread_.joinable()) {
            commandThread_.join();
        }
    }
    
    void stop() {
        running_.store(false);
        server_.stop();
    }

private:
    void commandLoop() {
        std::string input;
        
        while (running_.load()) {
            std::cout << "\n> ";
            std::getline(std::cin, input);
            
            if (input.empty()) {
                showStatus();
                continue;
            }
            
            char cmd = input[0];
            switch (cmd) {
                case 't':
                case 'T':
                    sendLargeText();
                    break;
                case 'b':
                case 'B':
                    sendLargeBinary();
                    break;
                case 'q':
                case 'Q':
                    std::cout << "Shutting down..." << std::endl;
                    running_.store(false);
                    return;
                default:
                    std::cout << "Unknown command: " << cmd << std::endl;
                    break;
            }
        }
    }
    
    void sendLargeText() {
        size_t size = 50 * 1024 * 1024; // 50MB
        std::string text(size, 'A');
        
        std::cout << "Sending 50MB text to " << server_.getConnectionCount() << " clients..." << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        server_.broadcastText(text);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Text broadcast completed in " << duration.count() << "ms" << std::endl;
    }
    
    void sendLargeBinary() {
        size_t size = 50 * 1024 * 1024; // 50MB
        std::vector<uint8_t> binary(size, 0x42);
        
        std::cout << "Sending 50MB binary to " << server_.getConnectionCount() << " clients..." << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        server_.broadcastBinary(binary);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Binary broadcast completed in " << duration.count() << "ms" << std::endl;
    }
    
    void showStatus() {
        auto connections = server_.getConnections();
        std::cout << "Active connections: " << connections.size() << std::endl;
        for (auto id : connections) {
            std::cout << "  Connection ID: " << id << std::endl;
        }
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
    std::cout << "WebSocket Simple Server" << std::endl;
    std::cout << "======================" << std::endl;
    
    SimpleWebSocketServer server;
    
    try {
        server.start();
    } catch (const std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
    }
    
    server.stop();
    return 0;
}
