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
#include <winsock2.h>
#include <ws2tcpip.h>

namespace WebSocketSimple {

// Forward declarations
class Connection;
class Server;

// Message types - unified interface (same as websocket_simple_cross)
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

// Connection class - represents a single WebSocket connection
class Connection {
public:
    Connection(SOCKET sock, uint64_t id);
    ~Connection();
    
    // Core operations - simple interface
    bool send(const Message& msg);
    bool sendText(const std::string& text);
    bool sendBinary(const std::vector<uint8_t>& data);
    
    // Callback registration
    void onMessage(MessageCallback callback);
    void onError(ErrorCallback callback);
    void onClose(CloseCallback callback);
    
    // Status
    bool isActive() const { return active_.load(); }
    uint64_t getId() const { return id_; }
    
    // Close connection
    void close();

private:
    SOCKET socket_;
    uint64_t id_;
    std::atomic<bool> active_;
    
    // Callbacks
    MessageCallback messageCallback_;
    ErrorCallback errorCallback_;
    CloseCallback closeCallback_;
    mutable std::mutex callbackMutex_;
    
    // Send queue and thread
    std::queue<std::vector<uint8_t>> sendQueue_;
    std::mutex sendMutex_;
    std::condition_variable sendCondition_;
    std::thread sendThread_;
    
    // Receive thread
    std::thread receiveThread_;
    
    // Fragment handling
    std::vector<uint8_t> fragmentBuffer_;
    Message::Type fragmentType_;
    bool hasFragment_;
    
    // Internal methods
    void sendLoop();
    void receiveLoop();
    bool sendRaw(const std::vector<uint8_t>& data);
    void handleFrame(const std::vector<uint8_t>& frame);
    size_t parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset);
    void notifyError(const std::string& error);
    void notifyClose();
};

// Server class - manages multiple connections
class Server {
public:
    Server();
    ~Server();
    
    // Core operations
    bool start(int port);
    void stop();
    
    // Broadcasting - simple interface
    void broadcast(const Message& msg);
    void broadcastText(const std::string& text);
    void broadcastBinary(const std::vector<uint8_t>& data);
    
    // Connection management
    std::vector<uint64_t> getConnections() const;
    size_t getConnectionCount() const;
    
    // Server callbacks
    void onConnection(std::function<void(std::shared_ptr<Connection>)> callback);
    void onError(std::function<void(const std::string&)> callback);

private:
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::thread acceptThread_;
    
    // Connection management
    std::map<uint64_t, std::shared_ptr<Connection>> connections_;
    mutable std::mutex connectionsMutex_;
    uint64_t nextConnectionId_;
    
    // Server callbacks
    std::function<void(std::shared_ptr<Connection>)> connectionCallback_;
    std::function<void(const std::string&)> errorCallback_;
    mutable std::mutex callbackMutex_;
    
    // Internal methods
    void acceptLoop();
    void removeConnection(uint64_t id);
    void notifyError(const std::string& error);
    bool performHandshake(SOCKET clientSocket);
    std::string simpleSHA1(const std::string& input);
    std::string base64Encode(const std::string& input);
};

} // namespace WebSocketSimple
