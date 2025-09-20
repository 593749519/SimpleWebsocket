#include "websocket_simple_cross.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <csignal>

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
                              << ": " << formatBytes(msg.size());
                    
                    // Show content only for small messages
                    if (msg.size() < 1000) {
                        std::cout << " - Content: " << msg.getTextData();
                    }
                    std::cout << std::endl;
                    
                    // Send echo response - return full data for large messages
                    if (msg.size() > 1024) {
                        // For large messages, echo the full data
                        conn->sendText(msg.getTextData());
                    } else {
                        // For small messages, send echo response with original content
                        std::string response = "Echo: " + msg.getTextData();
                        conn->sendText(response);
                    }
                    
                } else {
                    std::cout << "Server received binary from connection " << conn->getId() 
                              << ": " << formatBytes(msg.size());
                    
                    // Show content only for small binary messages
                    if (msg.size() < 100) {
                        std::cout << " - Content: ";
                        const auto& binaryData = msg.getBinaryData();
                        for (size_t i = 0; i < binaryData.size() && i < 20; ++i) {
                            printf("0x%02X ", binaryData[i]);
                        }
                        if (binaryData.size() > 20) {
                            std::cout << "...";
                        }
                    }
                    std::cout << std::endl;
                    
                    // Send echo response for binary - return full data for large messages
                    if (msg.size() > 1024) {
                        // For large binary messages, echo the full data
                        conn->sendBinary(msg.getBinaryData());
                    } else {
                        // For small binary messages, send echo response with original content
                        const auto& binaryData = msg.getBinaryData();
                        std::string response = "Echo: ";
                        response.append(binaryData.begin(), binaryData.end());
                        std::vector<uint8_t> binaryResponse(response.begin(), response.end());
                        conn->sendBinary(binaryResponse);
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
        running_.store(true);
        
        // Start command thread
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
    std::string formatBytes(size_t bytes) {
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
    
    void commandLoop() {
        std::string input;
        
        std::cout << "\n=== WebSocket Server Commands ===" << std::endl;
        std::cout << "t - Broadcast 50MB text to all clients" << std::endl;
        std::cout << "b - Broadcast 50MB binary to all clients" << std::endl;
        std::cout << "s - Broadcast small text message" << std::endl;
        std::cout << "d - Broadcast small binary data" << std::endl;
        std::cout << "c - Show connection count" << std::endl;
        std::cout << "q - Quit server" << std::endl;
        std::cout << "=================================" << std::endl;
        
        while (running_.load()) {
            std::cout << "\nEnter command: ";
            std::getline(std::cin, input);
            
            if (input == "q" || input == "Q") {
                break;
            } else if (input == "t" || input == "T") {
                broadcastLargeText();
            } else if (input == "b" || input == "B") {
                broadcastLargeBinary();
            } else if (input == "s" || input == "S") {
                broadcastSmallText();
            } else if (input == "d" || input == "D") {
                broadcastSmallBinary();
            } else if (input == "c" || input == "C") {
                showConnections();
            } else {
                std::cout << "Unknown command: " << input << std::endl;
            }
        }
        
        running_.store(false);
    }
    
    void broadcastLargeText() {
        std::cout << "Broadcasting 50MB text..." << std::endl;
        
        // Generate 50MB of text data
        std::string text(50 * 1024 * 1024, 'A');
        for (size_t i = 0; i < text.length(); i += 100) {
            text[i] = 'A' + (i / 100) % 26;
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        server_.broadcastText(text);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Broadcast completed in " << duration.count() << "ms" << std::endl;
        std::cout << "Sent to " << server_.getConnectionCount() << " clients" << std::endl;
    }
    
    void broadcastLargeBinary() {
        std::cout << "Broadcasting 50MB binary..." << std::endl;
        
        // Generate 50MB of binary data
        std::vector<uint8_t> binary(50 * 1024 * 1024);
        for (size_t i = 0; i < binary.size(); ++i) {
            binary[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        server_.broadcastBinary(binary);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Broadcast completed in " << duration.count() << "ms" << std::endl;
        std::cout << "Sent to " << server_.getConnectionCount() << " clients" << std::endl;
    }
    
    void broadcastSmallText() {
        std::string text = "Hello from cross-platform WebSocket server!";
        server_.broadcastText(text);
        std::cout << "Broadcasted small text: " << text << std::endl;
    }
    
    void broadcastSmallBinary() {
        std::vector<uint8_t> binary = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64};
        server_.broadcastBinary(binary);
        std::cout << "Broadcasted small binary data (" << binary.size() << " bytes)" << std::endl;
    }
    
    void showConnections() {
        size_t count = server_.getConnectionCount();
        std::cout << "Active connections: " << count << std::endl;
        
        if (count > 0) {
            auto connections = server_.getConnections();
            std::cout << "Connection IDs: ";
            for (size_t i = 0; i < connections.size(); ++i) {
                std::cout << connections[i];
                if (i < connections.size() - 1) {
                    std::cout << ", ";
                }
            }
            std::cout << std::endl;
        }
    }
};

int main() {
    std::cout << "WebSocket Cross-Platform Server" << std::endl;
    std::cout << "===============================" << std::endl;
    
    SimpleWebSocketServer server;
    
    // Handle Ctrl+C gracefully
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down server..." << std::endl;
        exit(0);
    });
    
    try {
        server.start();
    } catch (const std::exception& e) {
        std::cout << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
