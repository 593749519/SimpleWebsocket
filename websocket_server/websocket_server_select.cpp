#include "../public/websocket_simple_cross.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace WebSocketSimple;

class SelectBasedWebSocketServer {
private:
    std::unique_ptr<SelectBasedServer> server_;
    std::atomic<bool> running_;
    
public:
    SelectBasedWebSocketServer() : running_(false) {
        server_ = std::make_unique<SelectBasedServer>();
    }
    
    ~SelectBasedWebSocketServer() {
        stop();
    }
    
    bool start(int port) {
        if (running_.load()) {
            return false;
        }
        
        // Set up server callbacks
        server_->onConnection([this](std::shared_ptr<SelectBasedConnection> conn) {
            std::cout << "New connection: " << conn->getId() << std::endl;
            
            // Set up connection callbacks
            conn->onMessage([this, conn](const Message& msg) {
                handleMessage(conn, msg);
            });
            
            conn->onError([this, conn](const std::string& error) {
                std::cout << "Connection " << conn->getId() << " error: " << error << std::endl;
            });
            
            conn->onClose([this, conn]() {
                std::cout << "Connection " << conn->getId() << " closed" << std::endl;
            });
            
            conn->onPing([this, conn](const std::vector<uint8_t>& data) {
                std::cout << "[PING] Received PING from connection " << conn->getId() 
                         << " with " << data.size() << " bytes" << std::endl;
                // Send PONG response
                conn->sendPongFrame(data);
            });
            
            conn->onPong([this, conn](const std::vector<uint8_t>& data) {
                std::cout << "[PONG] Received PONG from connection " << conn->getId() 
                         << " with " << data.size() << " bytes" << std::endl;
            });
        });
        
        server_->onError([this](const std::string& error) {
            std::cout << "Server error: " << error << std::endl;
        });
        
        if (!server_->start(port)) {
            std::cout << "Failed to start SelectBasedServer on port " << port << std::endl;
            return false;
        }
        
        running_.store(true);
        std::cout << "SelectBasedServer started on port " << port << std::endl;
        return true;
    }
    
    void stop() {
        if (running_.exchange(false)) {
            server_->stop();
            std::cout << "SelectBasedServer stopped" << std::endl;
        }
    }
    
    void handleMessage(std::shared_ptr<SelectBasedConnection> conn, const Message& msg) {
        // Echo the message back to the client
        if (msg.type == Message::TEXT) {
            // For large messages, echo the full data; for small messages, send a simple response
            if (msg.size() > 1024) {
                // Large message - echo the full data
                conn->sendText(msg.getTextData());
                std::cout << "Server received text from connection " << conn->getId() 
                         << ": " << (msg.size() / (1024.0 * 1024.0)) << " MB" << std::endl;
            } else {
                // Small message - send a simple response
                std::string response = "Echo: " + msg.getTextData();
                conn->sendText(response);
                std::cout << "Server received text from connection " << conn->getId() 
                         << ": " << msg.size() << " B - Content: " << msg.getTextData() << std::endl;
            }
        } else if (msg.type == Message::BINARY) {
            // For large messages, echo the full data; for small messages, send a simple response
            if (msg.size() > 1024) {
                // Large message - echo the full data - use optimal access method
                conn->sendBinary(msg.getBinaryData());
                std::cout << "Server received binary from connection " << conn->getId() 
                         << ": " << (msg.size() / (1024.0 * 1024.0)) << " MB" << std::endl;
            } else {
                // Small message - send a simple response
                const auto& binaryData = msg.getBinaryData();
                std::string response = "Echo: ";
                response.append(binaryData.begin(), binaryData.end());
                std::vector<uint8_t> binaryResponse(response.begin(), response.end());
                conn->sendBinary(binaryResponse);
                std::cout << "Server received binary from connection " << conn->getId() 
                         << ": " << msg.size() << " B - Content: ";
                for (size_t i = 0; i < (binaryData.size() < 16 ? binaryData.size() : 16); ++i) {
                    printf("0x%02X ", binaryData[i]);
                }
                if (binaryData.size() > 16) std::cout << "...";
                std::cout << std::endl;
            }
        }
    }
    
    void runConsole() {
        std::cout << "\n=== SelectBasedServer Console ===" << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  't' - Broadcast large text data (50MB)" << std::endl;
        std::cout << "  'b' - Broadcast large binary data (50MB)" << std::endl;
        std::cout << "  'c' - Show connection count" << std::endl;
        std::cout << "  'q' - Quit server" << std::endl;
        std::cout << "=================================" << std::endl;
        
        std::string input;
        while (running_.load()) {
            std::cout << "Enter command: ";
            std::getline(std::cin, input);
            
            if (input == "q") {
                break;
            } else if (input == "t") {
                // Generate and broadcast large text data
                std::string largeText(50 * 1024 * 1024, 'A'); // 50MB of 'A'
                server_->broadcastText(largeText);
                std::cout << "Broadcasted 50MB text data to " << server_->getConnectionCount() << " clients" << std::endl;
            } else if (input == "b") {
                // Generate and broadcast large binary data
                std::vector<uint8_t> largeBinary(50 * 1024 * 1024, 0x42); // 50MB of 0x42
                server_->broadcastBinary(largeBinary);
                std::cout << "Broadcasted 50MB binary data to " << server_->getConnectionCount() << " clients" << std::endl;
            } else if (input == "c") {
                std::cout << "Active connections: " << server_->getConnectionCount() << std::endl;
            } else {
                std::cout << "Unknown command: " << input << std::endl;
            }
        }
    }
};

int main() {
    std::cout << "SelectBased WebSocket Server - High Performance Version" << std::endl;
    std::cout << "Using select-based I/O multiplexing for better performance" << std::endl;
    
    SelectBasedWebSocketServer server;
    
    if (!server.start(9001)) {
        std::cout << "Failed to start server" << std::endl;
        return 1;
    }
    
    // Run console interface
    server.runConsole();
    
    server.stop();
    return 0;
}
