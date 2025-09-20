#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <map>
#include <chrono>
#include "platform_socket.h"
#include "websocket_constants.h"

namespace WebSocketSimple {

// Forward declarations
class Connection;
class Server;

// Message types - unified interface (same as original)
struct Message {
    enum Type { TEXT, BINARY };
    Type type;
    
    // Separate storage for optimal performance - no data conversion needed
    std::string textData;
    std::vector<uint8_t> binaryData;
    
    // Constructors - zero copy for optimal performance
    Message(const std::string& text) : type(TEXT), textData(text) {}
    Message(std::string&& text) : type(TEXT), textData(std::move(text)) {}
    Message(const std::vector<uint8_t>& binary) : type(BINARY), binaryData(binary) {}
    Message(std::vector<uint8_t>&& binary) : type(BINARY), binaryData(std::move(binary)) {}
    
    // Access interfaces - type safe
    const std::string& getTextData() const { 
        return textData; 
    }
    const std::vector<uint8_t>& getBinaryData() const { 
        return binaryData; 
    }
    
    // Size access
    size_t size() const {
        return (type == TEXT) ? textData.size() : binaryData.size();
    }
};

// Connection callbacks - simple and clean
using MessageCallback = std::function<void(const Message&)>;
using ErrorCallback = std::function<void(const std::string&)>;
using CloseCallback = std::function<void()>;
using PingCallback = std::function<void(const std::vector<uint8_t>&)>;
using PongCallback = std::function<void(const std::vector<uint8_t>&)>;

// Connection class - represents a single WebSocket connection
class Connection {
public:
    Connection(platform_socket_t sock, uint64_t id);
    ~Connection();
    
    // Core operations - simple interface (same as original)
    bool send(const Message& msg);
    bool sendText(const std::string& text);
    bool sendBinary(const std::vector<uint8_t>& data);
    
    // Callback registration
    void onMessage(MessageCallback callback);
    void onError(ErrorCallback callback);
    void onClose(CloseCallback callback);
    void onPing(PingCallback callback);
    void onPong(PongCallback callback);
    
    // Status
    bool isActive() const { return active_.load(); }
    uint64_t getId() const { return id_; }
    platform_socket_t getSocket() const { return socket_; }
    
    // Automatic PING configuration
    void setAutoPing(int intervalSeconds = Network::DEFAULT_PING_INTERVAL_SECONDS); // 0 = disabled
    void setPongTimeout(int timeoutSeconds = Network::DEFAULT_PONG_TIMEOUT_SECONDS);
    
    // Close connection (same as original)
    void close();

public:
    // Handshake methods (moved from client/server classes)
    bool performClientHandshake(platform_socket_t sock, const std::string& host, int port);
    bool performServerHandshake(platform_socket_t clientSocket);

protected:
    // Make these methods accessible to SelectBasedConnection
    void handleFrame(const std::vector<uint8_t>& frame);
    size_t parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset);
    
    // Handshake helper methods
    std::string generateWebSocketKey();
    std::string calculateWebSocketAccept(const std::string& key);
    std::string base64Encode(const std::vector<uint8_t>& data);
    std::string simpleSHA1(const std::string& input);

private:
    platform_socket_t socket_;  // Changed from SOCKET to platform_socket_t
    uint64_t id_;
    std::atomic<bool> active_;
    
    // Callbacks
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    CloseCallback closeCallback_;
    PingCallback pingCallback_;
    PongCallback pongCallback_;
    mutable std::mutex callbackMutex_;
    
    // Send queue and thread (same as original)
    std::queue<std::vector<uint8_t>> sendQueue_;
    std::mutex sendMutex_;
    std::condition_variable sendCondition_;
    std::thread sendThread_;
    
    // Receive thread (same as original)
    std::thread receiveThread_;
    
    // Fragment handling
    std::vector<uint8_t> fragmentBuffer_;
    Message::Type fragmentType_;
    bool hasFragment_;
    mutable std::mutex fragmentMutex_;
    
    // Automatic PING state
    int pingIntervalSeconds_;
    int pongTimeoutSeconds_;
    std::chrono::steady_clock::time_point lastPingTime_;
    std::chrono::steady_clock::time_point lastPongTime_;
    bool waitingForPong_;
    
    // Internal methods
    void sendLoop();
    void receiveLoop();
    bool sendRaw(const std::vector<uint8_t>& data);
    void notifyError(const std::string& error);
    void notifyClose();
    
    // Control frame methods
    void sendPongFrame(const std::vector<uint8_t>& payload);
    void sendPingFrame(const std::vector<uint8_t>& payload = {});
    void handleCloseFrame(const std::vector<uint8_t>& payload);
    void sendCloseFrame(uint16_t code, const std::string& reason);
};

// Server class - manages multiple connections
class Server {
public:
    Server();
    ~Server();
    
    // Core operations (same as original)
    bool start(int port);
    void stop();
    
    // Broadcasting - simple interface (same as original)
    void broadcast(const Message& msg);
    void broadcastText(const std::string& text);
    void broadcastBinary(const std::vector<uint8_t>& data);
    
    // Connection management (same as original)
    std::vector<uint64_t> getConnections() const;
    size_t getConnectionCount() const;
    
    // Server callbacks (same as original)
    void onConnection(std::function<void(std::shared_ptr<Connection>)> callback);
    void onError(std::function<void(const std::string&)> callback);

private:
    platform_socket_t listenSocket_;  // Changed from SOCKET to platform_socket_t
    std::atomic<bool> running_;
    std::thread acceptThread_;
    
    // Connection management (same as original)
    std::map<uint64_t, std::shared_ptr<Connection>> connections_;
    mutable std::mutex connectionsMutex_;
    uint64_t nextConnectionId_;
    
    // Server callbacks (same as original)
    std::function<void(std::shared_ptr<Connection>)> connectionCallback_;
    std::function<void(const std::string&)> errorCallback_;
    mutable std::mutex callbackMutex_;
    
    // Internal methods (same as original)
    void acceptLoop();
    void removeConnection(uint64_t id);
    void notifyError(const std::string& error);
    bool performHandshake(platform_socket_t clientSocket);  // Changed parameter type
    std::string simpleSHA1(const std::string& input);
    std::string base64Encode(const std::string& input);
};

// Select-based Connection for high-performance server
class SelectBasedConnection {
public:
    SelectBasedConnection(platform_socket_t sock, uint64_t id);
    ~SelectBasedConnection();
    
    // Basic properties
    platform_socket_t getSocket() const { return socket_; }
    uint64_t getId() const { return id_; }
    bool isActive() const { return active_.load(); }
    
    // Message sending (non-blocking, called from select loop)
    bool sendText(const std::string& text);
    bool sendBinary(const std::vector<uint8_t>& data);
    bool send(const Message& msg);
    void sendPongFrame(const std::vector<uint8_t>& payload);
    
    // Callbacks (same interface as Connection for compatibility)
    void onMessage(std::function<void(const Message&)> callback);
    void onError(std::function<void(const std::string&)> callback);
    void onClose(std::function<void()> callback);
    void onPing(std::function<void(const std::vector<uint8_t>&)> callback);
    void onPong(std::function<void(const std::vector<uint8_t>&)> callback);
    
    // Data processing (called from select loop)
    void processReceivedData(const char* data, size_t length);
    bool hasDataToSend() const;
    std::vector<uint8_t> getDataToSend();
    void putDataBackToQueue(const std::vector<uint8_t>& data);
    void putRemainingDataBack(const std::vector<uint8_t>& remaining);
    bool hasPongData() const;
    
    // PING/PONG methods
    void setPingInterval(int seconds);
    void setPongTimeout(int seconds);
    void sendPingFrame(const std::vector<uint8_t>& payload = {});
    void checkPingTimeout();
    
    // Connection management
    void close();
    
    // Handshake methods (same as Connection class)
    bool performServerHandshake(platform_socket_t clientSocket);
    std::string simpleSHA1(const std::string& input);
    std::string base64Encode(const std::string& input);

private:
    platform_socket_t socket_;
    uint64_t id_;
    std::atomic<bool> active_;
    
    // Send queue (non-blocking)
    std::queue<std::vector<uint8_t>> sendQueue_;
    mutable std::mutex sendQueueMutex_;
    
    // PONG response queue (to avoid interfering with large message sending)
    std::queue<std::vector<uint8_t>> pongQueue_;
    mutable std::mutex pongQueueMutex_;
    
    // Receive buffer (for frame assembly)
    std::vector<uint8_t> receiveBuffer_;
    mutable std::mutex receiveBufferMutex_;
    bool hasFragment_;
    
    // Fragment assembly (for fragmented messages)
    std::vector<uint8_t> fragmentBuffer_;
    Message::Type fragmentType_;
    
    // Callbacks
    std::function<void(const Message&)> messageCallback_;
    std::function<void(const std::string&)> errorCallback_;
    std::function<void()> closeCallback_;
    std::function<void(const std::vector<uint8_t>&)> pingCallback_;
    std::function<void(const std::vector<uint8_t>&)> pongCallback_;
    mutable std::mutex callbackMutex_;
    
    // PING/PONG mechanism
    int pingIntervalSeconds_;
    int pongTimeoutSeconds_;
    std::atomic<bool> waitingForPong_;
    std::chrono::steady_clock::time_point lastPingTime_;
    
    // Internal methods
    void parseFrameFromBuffer();
    void notifyError(const std::string& error);
    void notifyClose();
    
    // Frame processing methods (copied from Connection for performance)
    size_t parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset);
    void handleFrame(const std::vector<uint8_t>& frame);
};

// High-performance Select-based Server for better performance
class SelectBasedServer {
public:
    SelectBasedServer();
    ~SelectBasedServer();
    
    // Same interface as original Server for compatibility
    bool start(int port);
    void stop();
    
    // Broadcasting - same interface
    void broadcast(const Message& msg);
    void broadcastText(const std::string& text);
    void broadcastBinary(const std::vector<uint8_t>& data);
    
    // Connection management - same interface
    std::vector<uint64_t> getConnections() const;
    size_t getConnectionCount() const;
    
    // Server callbacks - same interface
    void onConnection(std::function<void(std::shared_ptr<SelectBasedConnection>)> callback);
    void onError(std::function<void(const std::string&)> callback);

private:
    platform_socket_t listenSocket_;
    std::atomic<bool> running_;
    std::thread eventLoopThread_;
    
    // Connection management
    std::map<uint64_t, std::shared_ptr<SelectBasedConnection>> connections_;
    mutable std::mutex connectionsMutex_;
    uint64_t nextConnectionId_;
    
    
    // Server callbacks
    std::function<void(std::shared_ptr<SelectBasedConnection>)> connectionCallback_;
    std::function<void(const std::string&)> errorCallback_;
    mutable std::mutex callbackMutex_;
    
    // Select-based event loop
    void eventLoop();
    void removeConnection(uint64_t id);
    void notifyError(const std::string& error);
    bool performHandshake(platform_socket_t clientSocket);
    std::string simpleSHA1(const std::string& input);
    std::string base64Encode(const std::string& input);
    
    // Select-specific methods
    void setupSelectSets(fd_set& readfds, fd_set& writefds, int& max_fd);
    void handleSelectEvents(const fd_set& readfds, const fd_set& writefds);
    
    // Connection handling methods
    void handleNewConnection();
    void handleConnectionData(std::shared_ptr<SelectBasedConnection> conn, std::vector<uint64_t>& connectionsToRemove);
    void handleConnectionWrite(std::shared_ptr<SelectBasedConnection> conn, std::vector<uint64_t>& connectionsToRemove);
};

} // namespace WebSocketSimple
