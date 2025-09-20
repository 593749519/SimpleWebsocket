#include "../public/websocket_advanced_select.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>

using namespace WebSocketAdvanced;

// Global server instance for signal handling
std::unique_ptr<AdvancedSelectServer> g_server;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully..." << std::endl;
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    std::cout << "Advanced Select-Based WebSocket Server Starting..." << std::endl;
    std::cout << "Optimized for large data transfer with flow control" << std::endl;
    
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create server
    g_server = std::make_unique<AdvancedSelectServer>();
    
    // Configure server for large data transfer
    g_server->setMaxConnections(1000);
    g_server->setSelectTimeout(1000); // 1 second timeout
    g_server->setDefaultSendBufferSize(2 * 1024 * 1024); // 2MB buffer
    g_server->setDefaultMaxSendRate(10 * 1024 * 1024); // 10MB/s rate limit
    
    // Set up callbacks
    g_server->onConnection([](std::shared_ptr<AdvancedSelectConnection> conn) {
        std::cout << "New connection: " << conn->getId() << std::endl;
        
        // Configure connection for large data transfer
        conn->setSendBufferSize(2 * 1024 * 1024); // 2MB send buffer
        conn->setMaxSendRate(10 * 1024 * 1024); // 10MB/s rate limit
        conn->setAutoPing(30); // 30 second ping interval
        conn->setPongTimeout(10); // 10 second pong timeout
        
        // Set up message callback
        conn->onMessage([conn](const Message& msg) {
            std::cout << "Connection " << conn->getId() 
                      << " received " << formatBytes(msg.size()) 
                      << " (" << (msg.type == Message::TEXT ? "TEXT" : "BINARY") << ")" << std::endl;
            
            // Echo the message back
            if (msg.type == Message::TEXT) {
                conn->sendText(msg.text);
            } else {
                conn->sendBinary(msg.binary);
            }
        });
        
        conn->onError([conn](const std::string& error) {
            std::cout << "Connection " << conn->getId() << " error: " << error << std::endl;
        });
        
        conn->onClose([conn]() {
            std::cout << "Connection " << conn->getId() << " closed" << std::endl;
        });
        
        conn->onPing([conn](const std::vector<uint8_t>& payload) {
            std::cout << "Connection " << conn->getId() << " received PING" << std::endl;
        });
        
        conn->onPong([conn](const std::vector<uint8_t>& payload) {
            std::cout << "Connection " << conn->getId() << " received PONG" << std::endl;
        });
    });
    
    g_server->onError([](const std::string& error) {
        std::cout << "Server error: " << error << std::endl;
    });
    
    // Start server
    if (!g_server->start(8080)) {
        std::cerr << "Failed to start server on port 8080" << std::endl;
        return 1;
    }
    
    std::cout << "Server started on port 8080" << std::endl;
    std::cout << "Press Ctrl+C to stop the server" << std::endl;
    
    // Keep server running
    while (g_server->isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Print statistics every 10 seconds
        static auto lastStatsTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastStatsTime > std::chrono::seconds(10)) {
            std::cout << "Stats - Connections: " << g_server->getConnectionCount()
                      << ", Sent: " << formatBytes(g_server->getTotalBytesSent())
                      << ", Received: " << formatBytes(g_server->getTotalBytesReceived()) << std::endl;
            lastStatsTime = now;
        }
    }
    
    std::cout << "Server stopped" << std::endl;
    return 0;
}

// Helper function to format bytes
std::string formatBytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return oss.str();
}
