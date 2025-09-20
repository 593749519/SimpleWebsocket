#include "websocket_simple.h"
#include "websocket_constants.h"
#include <iostream>
#include <algorithm>

namespace WebSocketSimple {

// Optimized unmasking function - batch processing for better performance
void unmaskPayloadOptimized(std::vector<uint8_t>& payload, const uint8_t* maskKey) {
    size_t size = payload.size();
    if (size == 0) return;
    
    uint8_t* data = payload.data();
    
    // Process 4-byte aligned chunks for better performance
    size_t alignedSize = (size / 4) * 4;
    for (size_t i = 0; i < alignedSize; i += 4) {
        uint32_t* chunk = reinterpret_cast<uint32_t*>(data + i);
        uint32_t mask = *reinterpret_cast<const uint32_t*>(maskKey);
        *chunk ^= mask;
    }
    
    // Process remaining bytes
    for (size_t i = alignedSize; i < size; ++i) {
        data[i] ^= maskKey[i % 4];
    }
}

// Connection implementation
Connection::Connection(SOCKET sock, uint64_t id) 
    : socket_(sock), id_(id), active_(true), hasFragment_(false) {
    
    // Optimize socket
    if (socket_ != INVALID_SOCKET) {
        int bufferSize = Network::SOCKET_BUFFER_SIZE;
        setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, (char*)&bufferSize, sizeof(bufferSize));
        setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (char*)&bufferSize, sizeof(bufferSize));
        
        int nodelay = Performance::TCP_NODELAY_ENABLE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
    }
    
    // Start threads
    sendThread_ = std::thread(&Connection::sendLoop, this);
    receiveThread_ = std::thread(&Connection::receiveLoop, this);
}

Connection::~Connection() {
    close();
}

void Connection::close() {
    if (active_.exchange(false)) {
        // Signal send thread to stop
        sendCondition_.notify_all();
        
        // Close socket
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        
        // Wait for threads
        if (sendThread_.joinable()) {
            sendThread_.join();
        }
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
        
        notifyClose();
    }
}

bool Connection::send(const Message& msg) {
    if (!active_.load() || socket_ == INVALID_SOCKET) {
        return false;
    }
    
    try {
    const size_t maxFrameSize = Network::MAX_FRAME_SIZE;
    size_t offset = 0;
    bool isFirstFrame = true;
    
        size_t dataSize = msg.size();
        while (offset < dataSize) {
            size_t frameSize = (maxFrameSize < (dataSize - offset)) ? maxFrameSize : (dataSize - offset);
            bool isLastFrame = (offset + frameSize >= dataSize);
        
        // Create WebSocket frame
        std::vector<uint8_t> frame;
        
        // First byte: FIN + opcode
            uint8_t firstByte = isLastFrame ? Protocol::FIN_FLAG : 0;
            if (isFirstFrame) {
                if (msg.type == Message::TEXT) {
                    firstByte |= Protocol::OPCODE_TEXT;
                } else {
                    firstByte |= Protocol::OPCODE_BINARY;
                }
            } else {
                firstByte |= Protocol::OPCODE_CONTINUATION;
            }
        frame.push_back(firstByte);
        
        // Second byte: payload length
        if (frameSize < 126) {
            frame.push_back(static_cast<uint8_t>(frameSize));
        } else if (frameSize < Network::FRAME_SIZE_THRESHOLD_16BIT) {
                frame.push_back(Protocol::PAYLOAD_LENGTH_16BIT);
            frame.push_back(static_cast<uint8_t>((frameSize >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(frameSize & 0xFF));
        } else {
                frame.push_back(Protocol::PAYLOAD_LENGTH_64BIT);
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<uint8_t>((frameSize >> (i * 8)) & 0xFF));
            }
        }
        
            // Payload data (fragment) - use appropriate data access method
            if (msg.type == Message::TEXT) {
                const auto& textData = msg.getTextData();
                frame.insert(frame.end(), textData.begin() + offset, textData.begin() + offset + frameSize);
            } else {
                const auto& binaryData = msg.getBinaryData();
                frame.insert(frame.end(), binaryData.begin() + offset, binaryData.begin() + offset + frameSize);
            }
        
        // Queue for sending
        {
                std::lock_guard<std::mutex> lock(sendMutex_);
                sendQueue_.push(frame);
        }
            sendCondition_.notify_one();
        
        offset += frameSize;
        isFirstFrame = false;
    }
    
    return true;
    } catch (...) {
        return false;
    }
}

bool Connection::sendText(const std::string& text) {
    return send(Message(text));
}

bool Connection::sendBinary(const std::vector<uint8_t>& data) {
    return send(Message(data));
}

void Connection::onMessage(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    messageCallback_ = callback;
}

void Connection::onError(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = callback;
}

void Connection::onClose(CloseCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    closeCallback_ = callback;
}

void Connection::sendLoop() {
    while (active_.load()) {
        std::vector<uint8_t> data;
        
        {
            std::unique_lock<std::mutex> lock(sendMutex_);
            sendCondition_.wait(lock, [this] { 
                return !sendQueue_.empty() || !active_.load(); 
            });
            
            if (!active_.load()) break;
            
            if (!sendQueue_.empty()) {
                data = sendQueue_.front();
                sendQueue_.pop();
            }
        }
        
        if (!data.empty()) {
            if (!sendRaw(data)) {
                active_.store(false);
                break;
            }
        }
    }
}

bool Connection::sendRaw(const std::vector<uint8_t>& data) {
    if (socket_ == INVALID_SOCKET || data.empty()) {
        return false;
    }
    
            size_t totalSent = 0;
    const char* ptr = reinterpret_cast<const char*>(data.data());
    
    while (totalSent < data.size()) {
        int result = ::send(socket_, ptr + totalSent, 
                    static_cast<int>(data.size() - totalSent), 0);
                
                if (result == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
            }
            return false;
        } else if (result == 0) {
            return false; // Connection closed
                    } else {
            totalSent += static_cast<size_t>(result);
        }
    }
    
    return true;
}

void Connection::receiveLoop() {
    std::vector<uint8_t> buffer(Network::RECEIVE_BUFFER_SIZE);
    std::vector<uint8_t> receiveBuffer; // Buffer for incomplete frames
    
    while (active_.load() && socket_ != INVALID_SOCKET) {
        int result = recv(socket_, reinterpret_cast<char*>(buffer.data()), 
                         static_cast<int>(buffer.size()), 0);
        
        if (result == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            notifyError("Receive error: " + std::to_string(error));
                        break;
                } else if (result == 0) {
            // Connection closed by peer
            break;
        } else {
            // Add received data to buffer
            receiveBuffer.insert(receiveBuffer.end(), buffer.begin(), buffer.begin() + result);
            
            // Process complete frames
            size_t processed = 0;
            while (processed < receiveBuffer.size()) {
                size_t frameSize = parseFrameSize(receiveBuffer, processed);
                if (frameSize == 0 || processed + frameSize > receiveBuffer.size()) {
                    // Incomplete frame, wait for more data
                    break;
                }
                
                // Extract complete frame
                std::vector<uint8_t> frame(receiveBuffer.begin() + processed, 
                                          receiveBuffer.begin() + processed + frameSize);
                handleFrame(frame);
                
                processed += frameSize;
            }
            
            // Remove processed data
            if (processed > 0) {
                receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + processed);
            }
        }
    }
    
    active_.store(false);
}

size_t Connection::parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset) {
    if (offset + 2 > buffer.size()) return 0;
    
    uint8_t secondByte = buffer[offset + 1];
    uint8_t payloadLength = secondByte & 0x7F;
    
    size_t headerSize = 2;
    
    // Extended payload length
    if (payloadLength == Protocol::PAYLOAD_LENGTH_16BIT) {
        if (offset + 4 > buffer.size()) return 0;
        headerSize = 4;
    } else if (payloadLength == Protocol::PAYLOAD_LENGTH_64BIT) {
        if (offset + 10 > buffer.size()) return 0;
        headerSize = 10;
    }
    
    // Masking key (client frames are masked)
    bool masked = (secondByte & Protocol::MASK_FLAG) != 0;
    if (masked) {
        headerSize += 4;
    }
    
    // Calculate total frame size
    size_t actualPayloadLength = payloadLength;
    if (payloadLength == Protocol::PAYLOAD_LENGTH_16BIT) {
        actualPayloadLength = (static_cast<uint16_t>(buffer[offset + 2]) << 8) | buffer[offset + 3];
    } else if (payloadLength == Protocol::PAYLOAD_LENGTH_64BIT) {
        actualPayloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            actualPayloadLength = (actualPayloadLength << 8) | buffer[offset + 2 + i];
        }
    }
    
    return headerSize + actualPayloadLength;
}

void Connection::handleFrame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) return;
    
    // Parse WebSocket frame
    uint8_t firstByte = frame[0];
    uint8_t secondByte = frame[1];
    
    bool fin = (firstByte & Protocol::FIN_FLAG) != 0;
    uint8_t opcode = firstByte & 0x0F;
    bool masked = (secondByte & Protocol::MASK_FLAG) != 0;
    uint8_t payloadLength = secondByte & 0x7F;
    
    size_t headerSize = 2;
    size_t actualPayloadLength = payloadLength;
    
    // Extended payload length
    if (payloadLength == Protocol::PAYLOAD_LENGTH_16BIT) {
        if (frame.size() < 4) return;
        actualPayloadLength = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        headerSize = 4;
    } else if (payloadLength == Protocol::PAYLOAD_LENGTH_64BIT) {
        if (frame.size() < 10) return;
        actualPayloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            actualPayloadLength = (actualPayloadLength << 8) | frame[2 + i];
        }
        headerSize = 10;
    }
    
    // Masking key
    if (masked) {
        headerSize += 4;
    }
    
    // Check if we have complete frame
    if (frame.size() < headerSize + actualPayloadLength) {
        return; // Incomplete frame
    }
    
    // Extract payload
    std::vector<uint8_t> payload(frame.begin() + headerSize, 
                                frame.begin() + headerSize + actualPayloadLength);
    
    // Unmask if necessary (optimized batch processing)
    if (masked) {
        std::vector<uint8_t> maskKey(frame.begin() + headerSize - 4, 
                                    frame.begin() + headerSize);
        unmaskPayloadOptimized(payload, maskKey.data());
    }
    
    // Handle fragmented messages
    if (opcode == Protocol::OPCODE_CONTINUATION) {
        if (!hasFragment_) {
            // Unexpected continuation frame
            return;
        }
        // Add to fragment buffer
        fragmentBuffer_.insert(fragmentBuffer_.end(), payload.begin(), payload.end());
        
        if (fin) {
            // Complete fragmented message
            Message msg = (fragmentType_ == Message::TEXT) ? 
                Message(std::string(fragmentBuffer_.begin(), fragmentBuffer_.end())) : 
                Message(fragmentBuffer_);
            fragmentBuffer_.clear();
            hasFragment_ = false;
            
            // Notify callback
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (messageCallback_) {
                    try {
                        messageCallback_(msg);
                    } catch (...) {
                        // Ignore callback exceptions
                    }
                }
            }
        }
    } else if (opcode == Protocol::OPCODE_TEXT || opcode == Protocol::OPCODE_BINARY) {
        Message::Type msgType = (opcode == Protocol::OPCODE_TEXT) ? Message::TEXT : Message::BINARY;
        
        if (fin) {
            // Complete single frame message
            Message msg = (msgType == Message::TEXT) ? 
                Message(std::string(payload.begin(), payload.end())) : 
                Message(payload);
            
            // Notify callback
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (messageCallback_) {
                    try {
                        messageCallback_(msg);
                    } catch (...) {
                        // Ignore callback exceptions
                    }
                }
            }
        } else {
            // Start of fragmented message
            fragmentBuffer_ = payload;
            fragmentType_ = msgType;
            hasFragment_ = true;
        }
    }
    // Ignore other opcodes (ping, pong, close)
}

void Connection::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {
            // Ignore callback exceptions
        }
    }
}

void Connection::notifyClose() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (closeCallback_) {
        try {
            closeCallback_();
        } catch (...) {
            // Ignore callback exceptions
        }
    }
}

// Server implementation
Server::Server() : listenSocket_(INVALID_SOCKET), running_(false), nextConnectionId_(1) {
    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

Server::~Server() {
    stop();
    WSACleanup();
}

bool Server::start(int port) {
    if (running_.load()) {
        return false;
    }
    
    // Create listen socket
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        return false;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    
    // Bind to port
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }
    
    // Start listening
    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }
    
    running_.store(true);
    acceptThread_ = std::thread(&Server::acceptLoop, this);
    
    return true;
}

void Server::stop() {
    if (running_.exchange(false)) {
        // Close listen socket
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        
        // Close all connections
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            for (auto& pair : connections_) {
                if (pair.second) {
                    pair.second->close();
                }
            }
            connections_.clear();
        }
        
        // Wait for accept thread
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
    }
}

void Server::broadcast(const Message& msg) {
    std::vector<std::shared_ptr<Connection>> connectionsCopy;
    
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (const auto& pair : connections_) {
            if (pair.second && pair.second->isActive()) {
                connectionsCopy.push_back(pair.second);
            }
        }
    }
    
    for (auto& conn : connectionsCopy) {
        conn->send(msg);
    }
}

void Server::broadcastText(const std::string& text) {
    broadcast(Message(text));
}

void Server::broadcastBinary(const std::vector<uint8_t>& data) {
    broadcast(Message(data));
}

std::vector<uint64_t> Server::getConnections() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::vector<uint64_t> ids;
    for (const auto& pair : connections_) {
        if (pair.second && pair.second->isActive()) {
            ids.push_back(pair.first);
        }
    }
    return ids;
}

size_t Server::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    size_t count = 0;
    for (const auto& pair : connections_) {
        if (pair.second && pair.second->isActive()) {
            count++;
        }
    }
    return count;
}

void Server::onConnection(std::function<void(std::shared_ptr<Connection>)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionCallback_ = callback;
}

void Server::onError(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = callback;
}

void Server::acceptLoop() {
    while (running_.load() && listenSocket_ != INVALID_SOCKET) {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        
        SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (running_.load()) {
                notifyError("Accept failed");
            }
            continue;
        }
    
    // Perform WebSocket handshake
    if (!performHandshake(clientSocket)) {
        closesocket(clientSocket);
            continue;
    }
    
    // Create connection
        uint64_t connectionId = nextConnectionId_++;
        auto connection = std::make_shared<Connection>(clientSocket, connectionId);
    
    // Add to connections map
    {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[connectionId] = connection;
        }
        
        // Set up connection callbacks
        connection->onClose([this, connectionId]() {
            removeConnection(connectionId);
        });
        
        // Notify server callback
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (connectionCallback_) {
                try {
                    connectionCallback_(connection);
                } catch (...) {
                    // Ignore callback exceptions
                }
            }
        }
    }
}

void Server::removeConnection(uint64_t id) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(id);
}

bool Server::performHandshake(SOCKET clientSocket) {
    // Receive handshake request
    char buffer[Network::HANDSHAKE_BUFFER_SIZE];
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived == SOCKET_ERROR || bytesReceived == 0) {
        return false;
    }
    
    buffer[bytesReceived] = '\0';
    std::string request(buffer, bytesReceived);
    
    // Parse WebSocket key
    std::string key;
    size_t keyPos = request.find("Sec-WebSocket-Key: ");
    if (keyPos != std::string::npos) {
        keyPos += 19; // Length of "Sec-WebSocket-Key: "
        size_t keyEnd = request.find("\r\n", keyPos);
        if (keyEnd != std::string::npos) {
            key = request.substr(keyPos, keyEnd - keyPos);
        }
    }
    
    if (key.empty()) {
        return false;
    }
    
    // Generate accept key
    std::string acceptKey = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    
    // Simple SHA1 hash (for demo purposes)
    std::string hash = simpleSHA1(acceptKey);
    std::string accept = base64Encode(hash);
    
    // Send handshake response
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n";
    response += "\r\n";
    
    int bytesSent = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
    return bytesSent != SOCKET_ERROR;
}

std::string Server::simpleSHA1(const std::string& input) {
    // Simple hash function for demo (not cryptographically secure)
    uint32_t hash = 0;
    for (char c : input) {
        hash = hash * 31 + static_cast<uint8_t>(c);
    }
    
    std::string result;
    for (int i = 0; i < 20; ++i) {
        result += static_cast<char>((hash >> (i % 4 * 8)) & 0xFF);
    }
    return result;
}

std::string Server::base64Encode(const std::string& input) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    
    for (size_t i = 0; i < input.length(); i += 3) {
        uint32_t value = 0;
        int count = 0;
        
        for (int j = 0; j < 3 && i + j < input.length(); ++j) {
            value = (value << 8) | static_cast<uint8_t>(input[i + j]);
            count++;
        }
        
        value <<= (3 - count) * 8;
        
        for (int j = 0; j < 4; ++j) {
            if (j <= count) {
                result += chars[(value >> (18 - j * 6)) & 0x3F];
            } else {
                result += '=';
            }
        }
    }
    
    return result;
}

void Server::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {
            // Ignore callback exceptions
        }
    }
}

} // namespace WebSocketSimple
