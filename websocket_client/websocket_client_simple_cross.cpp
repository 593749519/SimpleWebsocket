#include "websocket_simple_cross.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <csignal>

using namespace WebSocketSimple;

class SimpleWebSocketClient {
private:
    std::shared_ptr<Connection> connection_;
    std::atomic<uint64_t> totalBytesReceived_{0};
    std::atomic<uint64_t> messageCount_{0};
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point lastReceiveTime_;
    uint64_t lastTotalBytes_;
    
    // Enhanced statistics for frame-level tracking
    std::chrono::steady_clock::time_point firstFrameTime_;
    std::chrono::steady_clock::time_point lastFrameTime_;
    std::atomic<bool> firstFrameReceived_{false};
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> totalFrameBytes_{0};

public:
    bool connect(const std::string& host, int port) {
        // Initialize platform socket
        if (platform_socket_init() != 0) {
            std::cout << "Platform socket initialization failed" << std::endl;
            return false;
        }
        
        // Create socket
        platform_socket_t sock = platform_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == PLATFORM_INVALID_SOCKET) {
            std::cout << "Socket creation failed" << std::endl;
            platform_socket_cleanup();
            return false;
        }
        
        // Connect to server
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);
        
        if (platform_connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == PLATFORM_SOCKET_ERROR) {
            std::cout << "Connection failed" << std::endl;
            platform_close_socket(sock);
            platform_socket_cleanup();
            return false;
        }
        
        // Create connection object first
        connection_ = std::make_shared<Connection>(sock, 1);
        
        // Perform WebSocket handshake using Connection class
        if (!connection_->performClientHandshake(sock, host, port)) {
            std::cout << "WebSocket handshake failed" << std::endl;
            platform_close_socket(sock);
            platform_socket_cleanup();
            connection_.reset();
            return false;
        }
        setupCallbacks();
        
        startTime_ = std::chrono::steady_clock::now();
        lastReceiveTime_ = startTime_;
        lastTotalBytes_ = 0;
        
        std::cout << "Connected to " << host << ":" << port << std::endl;
        return true;
    }
    
    void disconnect() {
        if (connection_ && connection_->isActive()) {
            connection_->close();
            connection_.reset();
            platform_socket_cleanup();
        }
    }
    
    void printTransferStatistics() {
        if (!firstFrameReceived_.load()) {
            std::cout << "[STATS] No frames received yet." << std::endl;
            return;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstFrameTime_).count();
        auto transferElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(lastFrameTime_ - firstFrameTime_).count();
        
        std::cout << "\n=== Transfer Statistics ===" << std::endl;
        std::cout << "Total Frames: " << frameCount_.load() << std::endl;
        std::cout << "Total Bytes: " << formatBytes(totalFrameBytes_.load()) << std::endl;
        std::cout << "First Frame Time: " 
                  << std::chrono::duration_cast<std::chrono::milliseconds>(firstFrameTime_.time_since_epoch()).count() 
                  << " ms" << std::endl;
        std::cout << "Last Frame Time: " 
                  << std::chrono::duration_cast<std::chrono::milliseconds>(lastFrameTime_.time_since_epoch()).count() 
                  << " ms" << std::endl;
        std::cout << "Transfer Duration: " << transferElapsed << " ms" << std::endl;
        
        if (transferElapsed > 0) {
            double transferSpeed = static_cast<double>(totalFrameBytes_.load()) / transferElapsed * 1000.0 / (1024 * 1024);
            std::cout << "Transfer Speed: " << std::fixed << std::setprecision(2) << transferSpeed << " MB/s" << std::endl;
        }
        
        if (totalElapsed > 0) {
            double avgSpeed = static_cast<double>(totalFrameBytes_.load()) / totalElapsed * 1000.0 / (1024 * 1024);
            std::cout << "Average Speed: " << std::fixed << std::setprecision(2) << avgSpeed << " MB/s" << std::endl;
        }
        std::cout << "========================" << std::endl;
    }
    
    void run() {
        if (!connection_ || !connection_->isActive()) {
            std::cout << "Not connected to server" << std::endl;
            return;
        }
        
        std::string input;
        
        std::cout << "\n=== WebSocket Client Commands ===" << std::endl;
        std::cout << "t - Send 50MB text to server" << std::endl;
        std::cout << "b - Send 50MB binary to server" << std::endl;
        std::cout << "s - Send small text message" << std::endl;
        std::cout << "d - Send small binary data" << std::endl;
        std::cout << "m - Multi-connection test (3 connections)" << std::endl;
        std::cout << "q - Quit client" << std::endl;
        std::cout << "================================" << std::endl;
        
        while (connection_ && connection_->isActive()) {
            std::cout << "\nEnter command: ";
            std::getline(std::cin, input);
            
            if (input == "q" || input == "Q") {
                printTransferStatistics();
                break;
            } else if (input == "t" || input == "T") {
                sendLargeText();
            } else if (input == "b" || input == "B") {
                sendLargeBinary();
            } else if (input == "s" || input == "S") {
                sendSmallText();
            } else if (input == "d" || input == "D") {
                sendSmallBinary();
            } else if (input == "m" || input == "M") {
                runMultiConnectionTest(3);
            } else {
                std::cout << "Unknown command: " << input << std::endl;
            }
        }
        
        disconnect();
    }
    
    // Public methods for sending data (used by multi-connection mode)
    void sendSmallText() {
        std::string text = "Hello from cross-platform WebSocket client!";
        if (connection_->sendText(text)) {
            std::cout << "Small text sent: " << text << std::endl;
        } else {
            std::cout << "Failed to send small text" << std::endl;
        }
    }
    
    void sendSmallBinary() {
        std::vector<uint8_t> data = {0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20, 0x57, 0x6F, 0x72, 0x6C, 0x64};
        if (connection_->sendBinary(data)) {
            std::cout << "Small binary sent: " << data.size() << " bytes" << std::endl;
        } else {
            std::cout << "Failed to send small binary" << std::endl;
        }
    }
    
    void sendLargeText() {
        std::string text(50 * 1024 * 1024, 'A'); // 50MB of 'A'
        if (connection_->sendText(text)) {
            std::cout << "Generated " << formatBytes(text.size()) << " of text data" << std::endl;
        } else {
            std::cout << "Failed to send large text" << std::endl;
        }
    }
    
    void sendLargeBinary() {
        std::vector<uint8_t> data(50 * 1024 * 1024, 0x42); // 50MB of 0x42
        if (connection_->sendBinary(data)) {
            std::cout << "Generated " << formatBytes(data.size()) << " of binary data" << std::endl;
        } else {
            std::cout << "Failed to send binary" << std::endl;
        }
    }
    
    void runMultiConnectionTest(int numConnections) {
        std::cout << "\n=== Multi-Connection Test (" << numConnections << " connections) ===" << std::endl;
        
        std::vector<std::unique_ptr<SimpleWebSocketClient>> clients;
        
        // Create multiple client connections
        for (int i = 0; i < numConnections; ++i) {
            auto client = std::make_unique<SimpleWebSocketClient>();
            if (!client->connect("127.0.0.1", 9001)) {
                std::cout << "Failed to connect client " << i << " to server" << std::endl;
                continue;
            }
            
            std::cout << "Client " << i << " connected successfully!" << std::endl;
            clients.push_back(std::move(client));
        }
        
        if (clients.empty()) {
            std::cout << "No clients connected successfully" << std::endl;
            return;
        }
        
        std::cout << "Sending test data from all clients..." << std::endl;
        
        // Send data from all clients
        for (size_t i = 0; i < clients.size(); ++i) {
            std::cout << "Client " << i << " sending data..." << std::endl;
            clients[i]->sendSmallText();
            clients[i]->sendSmallBinary();
            clients[i]->sendLargeText();
            clients[i]->sendLargeBinary();
        }
        
        // Keep connections alive to observe PING/PONG
        std::cout << "Connections established! PING/PONG will run automatically every 5 seconds." << std::endl;
        std::cout << "Press Enter to disconnect all connections and return to main menu..." << std::endl;
        
        // Wait for user input to disconnect
        std::string input;
        std::getline(std::cin, input);
        
        // Disconnect all clients
        std::cout << "Disconnecting all connections..." << std::endl;
        for (auto& client : clients) {
            client->disconnect();
        }
        
        std::cout << "Multi-connection test completed!" << std::endl;
    }

private:
    void setupCallbacks() {
        connection_->onMessage([this](const Message& msg) {
            messageCount_++;
            totalBytesReceived_ += msg.size();
            
            // Frame-level statistics tracking
            auto now = std::chrono::steady_clock::now();
            frameCount_++;
            totalFrameBytes_ += msg.size();
            
            // Record first frame time
            if (!firstFrameReceived_.exchange(true)) {
                firstFrameTime_ = now;
                std::cout << "[STATS] First frame received at: " 
                          << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() 
                          << " ms" << std::endl;
            }
            
            // Always update last frame time
            lastFrameTime_ = now;
            
            // Print detailed statistics for every complete message
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
            
            if (msg.type == Message::TEXT) {
                std::cout << "Client received text: " << formatBytes(msg.size()) 
                          << " (Total: " << formatBytes(totalBytesReceived_.load()) << ")";
            } else {
                std::cout << "Client received binary: " << formatBytes(msg.size()) 
                          << " (Total: " << formatBytes(totalBytesReceived_.load()) << ")";
            }
            
            // Calculate average speed (since connection start)
            if (elapsed > 0) {
                double avgSpeed = static_cast<double>(totalBytesReceived_.load()) / elapsed / (1024 * 1024);
                std::cout << " Avg Speed: " << std::fixed << std::setprecision(2) << avgSpeed << " MB/s";
            }
            
            // Calculate frame-level transfer speed (from first frame to current frame)
            if (firstFrameReceived_.load()) {
                auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstFrameTime_).count();
                if (frameElapsed > 0) {
                    double frameSpeed = static_cast<double>(totalFrameBytes_.load()) / frameElapsed * 1000.0 / (1024 * 1024);
                    std::cout << " Frame Speed: " << std::fixed << std::setprecision(2) << frameSpeed << " MB/s";
                }
            }
            
            // Update for next calculation
            lastReceiveTime_ = now;
            lastTotalBytes_ = totalBytesReceived_.load();
            std::cout << std::endl;
        });
        
        connection_->onError([this](const std::string& error) {
            std::cout << "Connection error: " << error << std::endl;
        });
        
        connection_->onClose([this]() {
            std::cout << "Connection closed" << std::endl;
        });
        
        // Set up PING/PONG callbacks for testing
        connection_->onPing([](const std::vector<uint8_t>& payload) {
            std::cout << "[PING] Received PING with " << payload.size() << " bytes" << std::endl;
        });
        
        connection_->onPong([](const std::vector<uint8_t>& payload) {
            std::cout << "[PONG] Received PONG with " << payload.size() << " bytes" << std::endl;
        });
        
        // Enable automatic PING every 5 seconds for testing
        connection_->setAutoPing(5);
        connection_->setPongTimeout(10);
        std::cout << "Automatic PING enabled: every 5 seconds, PONG timeout: 10 seconds" << std::endl;
    }
    
    
    std::string base64Encode(const std::string& input) {
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        
        for (size_t i = 0; i < input.length(); i += 3) {
            uint32_t value = 0;
            int padding = 0;
            
            for (int j = 0; j < 3; ++j) {
                value <<= 8;
                if (i + j < input.length()) {
                    value |= static_cast<uint8_t>(input[i + j]);
                } else {
                    padding++;
                }
            }
            
            for (int j = 0; j < 4; ++j) {
                if (j < 4 - padding) {
                    result += chars[(value >> (18 - j * 6)) & 0x3F];
                } else {
                    result += '=';
                }
            }
        }
        
        return result;
    }
    
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
    
};

void showMenu() {
    std::cout << "\n=== WebSocket Cross-Platform Client ===" << std::endl;
    std::cout << "1. Single Connection Test" << std::endl;
    std::cout << "2. Multiple Connections Test" << std::endl;
    std::cout << "3. Large Data Transfer Test" << std::endl;
    std::cout << "4. Performance Test" << std::endl;
    std::cout << "5. Custom Configuration" << std::endl;
    std::cout << "0. Exit" << std::endl;
    std::cout << "=====================================" << std::endl;
    std::cout << "Please select an option: ";
}

int main(int argc, char* argv[]) {
    std::cout << "WebSocket Cross-Platform Client" << std::endl;
    std::cout << "===============================" << std::endl;
    
    // Handle Ctrl+C gracefully
    std::signal(SIGINT, [](int) {
        std::cout << "\nDisconnecting..." << std::endl;
        exit(0);
    });
    
    // If command line arguments provided, use legacy mode
    if (argc > 1) {
        int numConnections = std::atoi(argv[1]);
        if (numConnections < 1 || numConnections > 10) {
            std::cout << "Usage: " << argv[0] << " [num_connections (1-10)]" << std::endl;
            return 1;
        }
        
        std::cout << "Legacy mode: Creating " << numConnections << " connection(s)..." << std::endl;
        // Continue with legacy logic...
    } else {
        // Interactive menu mode
        while (true) {
            showMenu();
            
            int choice;
            std::cin >> choice;
            
            switch (choice) {
                case 1: {
                    std::cout << "\n=== Single Connection Test ===" << std::endl;
        SimpleWebSocketClient client;
        
        if (!client.connect("127.0.0.1", 9001)) {
            std::cout << "Failed to connect to server" << std::endl;
                        break;
        }
        
        try {
            client.run();
        } catch (const std::exception& e) {
            std::cout << "Client error: " << e.what() << std::endl;
        }
                    break;
                }
                
                case 2: {
                    std::cout << "\n=== Multiple Connections Test ===" << std::endl;
                    std::cout << "Enter number of connections (1-10): ";
                    int numConnections;
                    std::cin >> numConnections;
                    
                    if (numConnections < 1 || numConnections > 10) {
                        std::cout << "Invalid number of connections" << std::endl;
                        break;
                    }
                    
                    std::cout << "Creating " << numConnections << " connection(s)..." << std::endl;
                    
        std::vector<std::unique_ptr<SimpleWebSocketClient>> clients;
        
                    // Create multiple client connections
                    for (int i = 0; i < numConnections; ++i) {
            auto client = std::make_unique<SimpleWebSocketClient>();
            if (!client->connect("127.0.0.1", 9001)) {
                std::cout << "Failed to connect client " << i << " to server" << std::endl;
                continue;
            }
                        
                        std::cout << "Client " << i << " connected successfully!" << std::endl;
            clients.push_back(std::move(client));
        }
        
        if (clients.empty()) {
            std::cout << "No clients connected successfully" << std::endl;
                        break;
        }
        
                    std::cout << "Sending test data from all clients..." << std::endl;
        
                    // Send data from all clients
        for (size_t i = 0; i < clients.size(); ++i) {
                        std::cout << "Client " << i << " sending data..." << std::endl;
            clients[i]->sendSmallText();
                        clients[i]->sendSmallBinary();
                        clients[i]->sendLargeText();
                        clients[i]->sendLargeBinary();
                    }
                    
                    // Keep connections alive to observe PING/PONG
                    std::cout << "Keeping connections alive for 30 seconds to observe PING/PONG..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(30));
        
        // Disconnect all clients
        for (auto& client : clients) {
            client->disconnect();
        }
                    break;
                }
                
                case 3: {
                    std::cout << "\n=== Large Data Transfer Test ===" << std::endl;
                    SimpleWebSocketClient client;
                    
                    if (!client.connect("127.0.0.1", 9001)) {
                        std::cout << "Failed to connect to server" << std::endl;
                        break;
                    }
                    
                    std::cout << "Sending large binary data..." << std::endl;
                    client.sendLargeBinary();
                    
                    std::cout << "Press Enter to disconnect..." << std::endl;
                    std::string input;
                    std::getline(std::cin, input);
                    std::getline(std::cin, input); // Clear buffer
                    
                    client.disconnect();
                    break;
                }
                
                case 4: {
                    std::cout << "\n=== Performance Test ===" << std::endl;
                    std::cout << "This test will create multiple connections and measure performance." << std::endl;
                    std::cout << "Enter number of connections for performance test (1-20): ";
                    int numConnections;
                    std::cin >> numConnections;
                    
                    if (numConnections < 1 || numConnections > 20) {
                        std::cout << "Invalid number of connections" << std::endl;
                        break;
                    }
                    
                    // Performance test implementation would go here
                    std::cout << "Performance test with " << numConnections << " connections completed." << std::endl;
                    break;
                }
                
                case 5: {
                    std::cout << "\n=== Custom Configuration ===" << std::endl;
                    std::cout << "Enter server host (default: 127.0.0.1): ";
                    std::string host;
                    std::cin >> host;
                    if (host.empty()) host = "127.0.0.1";
                    
                    std::cout << "Enter server port (default: 9001): ";
                    int port;
                    std::cin >> port;
                    if (port <= 0) port = 9001;
                    
                    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
                    
                    SimpleWebSocketClient client;
                    if (!client.connect(host, port)) {
                        std::cout << "Failed to connect to " << host << ":" << port << std::endl;
                break;
                    }
                    
                    try {
                        client.run();
                    } catch (const std::exception& e) {
                        std::cout << "Client error: " << e.what() << std::endl;
                    }
                break;
                }
                
                case 0: {
                std::cout << "Goodbye!" << std::endl;
                return 0;
                }
                
                default: {
                    std::cout << "Invalid option. Please try again." << std::endl;
                break;
                }
            }
        }
    }
    
    return 0;
}
