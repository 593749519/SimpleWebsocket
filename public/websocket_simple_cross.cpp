#include "websocket_simple_cross.h"
#include "websocket_constants.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <future>

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
Connection::Connection(platform_socket_t sock, uint64_t id) 
    : socket_(sock), id_(id), active_(true), hasFragment_(false),
      pingIntervalSeconds_(0), pongTimeoutSeconds_(Network::DEFAULT_PONG_TIMEOUT_SECONDS), waitingForPong_(false) {
    
    // Socket optimization is now handled at Server level for consistency
    
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
        
        // Close socket using platform abstraction
        if (socket_ != PLATFORM_INVALID_SOCKET) {
            platform_close_socket(socket_);
            socket_ = PLATFORM_INVALID_SOCKET;
        }
        
        // Wait for threads - but don't join from within the thread itself
        if (sendThread_.joinable() && sendThread_.get_id() != std::this_thread::get_id()) {
            sendThread_.join();
        }
        if (receiveThread_.joinable() && receiveThread_.get_id() != std::this_thread::get_id()) {
            receiveThread_.join();
        }
        
        notifyClose();
    }
}

// Handshake methods (moved from client/server classes)
bool Connection::performClientHandshake(platform_socket_t sock, const std::string& host, int port) {
    // Generate WebSocket key
    std::string key = generateWebSocketKey();
    
    // Send HTTP upgrade request
    std::string request = 
        "GET " + std::string(Handshake::DEFAULT_PATH) + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        + std::string(Handshake::HTTP_UPGRADE_HEADER) + "\r\n"
        + std::string(Handshake::HTTP_CONNECTION_HEADER) + "\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: " + std::string(Handshake::WEBSOCKET_VERSION) + "\r\n"
        "\r\n";
    
    if (platform_send(sock, request.c_str(), static_cast<int>(request.length()), 0) <= 0) {
        return false;
    }
    
    // Read response
    std::string response;
    char buffer[Network::HANDSHAKE_BUFFER_SIZE];
    
    while (true) {
        int result = platform_recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (result <= 0) {
            return false;
        }
        
        buffer[result] = '\0';
        response += buffer;
        
        if (response.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    
    // Check for 101 Switching Protocols
    if (response.find("101 Switching Protocols") == std::string::npos) {
        return false;
    }
    
    // Validate Sec-WebSocket-Accept
    std::string expectedAccept = calculateWebSocketAccept(key);
    std::string acceptHeader = std::string(Handshake::WEBSOCKET_ACCEPT_HEADER) + ": ";
    size_t acceptStart = response.find(acceptHeader);
    if (acceptStart == std::string::npos) {
        return false;
    }
    
    acceptStart += acceptHeader.length();
    size_t acceptEnd = response.find("\r\n", acceptStart);
    if (acceptEnd == std::string::npos) {
        return false;
    }
    
    std::string receivedAccept = response.substr(acceptStart, acceptEnd - acceptStart);
    return receivedAccept == expectedAccept;
}

bool Connection::performServerHandshake(platform_socket_t clientSocket) {
    // Read HTTP request
    std::string request;
    char buffer[Network::HANDSHAKE_BUFFER_SIZE];
    
    while (true) {
        int result = platform_recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (result <= 0) {
            return false;
        }
        
        buffer[result] = '\0';
        request += buffer;
        
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    
    // Parse Sec-WebSocket-Key
    std::string key;
    std::string keyHeader = std::string(Handshake::WEBSOCKET_KEY_HEADER) + ": ";
    size_t keyStart = request.find(keyHeader);
    if (keyStart != std::string::npos) {
        keyStart += keyHeader.length();
        size_t keyEnd = request.find("\r\n", keyStart);
        if (keyEnd != std::string::npos) {
            key = request.substr(keyStart, keyEnd - keyStart);
        }
    }
    
    if (key.empty()) {
        return false;
    }
    
    // Generate Sec-WebSocket-Accept
    std::string acceptKey = key + Handshake::WEBSOCKET_MAGIC_STRING;
    std::string acceptHash = simpleSHA1(acceptKey);
    std::vector<uint8_t> hashBytes(acceptHash.begin(), acceptHash.end());
    std::string acceptValue = base64Encode(hashBytes);
    
    // Send HTTP response
    std::string response = 
        std::string(Handshake::HTTP_SWITCHING_PROTOCOLS) + "\r\n"
        + std::string(Handshake::HTTP_UPGRADE_HEADER) + "\r\n"
        + std::string(Handshake::HTTP_CONNECTION_HEADER) + "\r\n"
        "Sec-WebSocket-Accept: " + acceptValue + "\r\n"
        "\r\n";
    
    int sent = platform_send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
    return sent > 0;
}

std::string Connection::generateWebSocketKey() {
    // Generate 16 random bytes
    std::vector<uint8_t> randomBytes(16);
    for (int i = 0; i < 16; ++i) {
        randomBytes[i] = static_cast<uint8_t>(rand() % 256);
    }
    
    // Base64 encode the random bytes
    return base64Encode(randomBytes);
}

std::string Connection::calculateWebSocketAccept(const std::string& key) {
    // SHA1(key + WEBSOCKET_MAGIC_STRING)
    std::string input = key + Handshake::WEBSOCKET_MAGIC_STRING;
    std::string hash = simpleSHA1(input);
    std::vector<uint8_t> hashBytes(hash.begin(), hash.end());
    return base64Encode(hashBytes);
}

std::string Connection::base64Encode(const std::vector<uint8_t>& data) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t value = 0;
        int padding = 0;
        
        for (int j = 0; j < 3; ++j) {
            value <<= 8;
            if (i + j < data.size()) {
                value |= data[i + j];
        } else {
                padding++;
            }
        }
        
        for (int j = 0; j < 4 - padding; ++j) {
            result += chars[(value >> (6 * (3 - j))) & 0x3F];
        }
        
        for (int j = 0; j < padding; ++j) {
            result += '=';
        }
    }
    
    return result;
}

std::string Connection::simpleSHA1(const std::string& input) {
    // Standard SHA1 implementation for WebSocket handshake
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    
    // Add padding
    std::string padded = input;
    padded += static_cast<char>(0x80);
    
    // Pad to 448 bits (56 bytes) mod 512
    while ((padded.length() % SHA1::BLOCK_SIZE) != SHA1::PADDING_MODULO) {
        padded += static_cast<char>(0x00);
    }
    
    // Append original length in bits as 64-bit big-endian
    uint64_t bitLength = input.length() * 8;
    for (int i = 7; i >= 0; --i) {
        padded += static_cast<char>((bitLength >> (i * 8)) & 0xFF);
    }
    
    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < padded.length(); chunk += SHA1::BLOCK_SIZE) {
        uint32_t w[80];
        
        // Break chunk into 16 32-bit big-endian words
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        
        // Extend the 16 32-bit words into 80 32-bit words
        for (int i = 16; i < 80; ++i) {
            w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        
        // Main loop
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
        } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    
    // Convert to string (big-endian)
    std::string result;
    for (int i = 0; i < 5; ++i) {
        result += static_cast<char>((h[i] >> 24) & 0xFF);
        result += static_cast<char>((h[i] >> 16) & 0xFF);
        result += static_cast<char>((h[i] >> 8) & 0xFF);
        result += static_cast<char>(h[i] & 0xFF);
    }
    
    return result;
}

bool Connection::send(const Message& msg) {
    if (!active_.load() || socket_ == PLATFORM_INVALID_SOCKET) {
        return false;
    }
    
    try {
        const size_t maxFrameSize = Network::MAX_FRAME_SIZE;
        size_t offset = 0;
        bool isFirstFrame = true;
        
        // Create all frames first, then add them to queue atomically
        std::vector<std::vector<uint8_t>> frames;
        
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
            
            // Payload - use appropriate data access method
            if (msg.type == Message::TEXT) {
                const auto& textData = msg.getTextData();
                frame.insert(frame.end(), textData.begin() + offset, textData.begin() + offset + frameSize);
            } else {
                const auto& binaryData = msg.getBinaryData();
                frame.insert(frame.end(), binaryData.begin() + offset, binaryData.begin() + offset + frameSize);
            }
            
            frames.push_back(std::move(frame));
            
            offset += frameSize;
            isFirstFrame = false;
        }
        
                // Add all frames to queue atomically
                {
                    std::lock_guard<std::mutex> lock(sendMutex_);
                    for (size_t i = 0; i < frames.size(); ++i) {
                        sendQueue_.push(std::move(frames[i]));
                    }
                }
        sendCondition_.notify_one();
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

void Connection::onPing(PingCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pingCallback_ = callback;
}

void Connection::onPong(PongCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pongCallback_ = callback;
}

void Connection::setAutoPing(int intervalSeconds) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pingIntervalSeconds_ = intervalSeconds;
    if (intervalSeconds > 0) {
        lastPingTime_ = std::chrono::steady_clock::now();
        lastPongTime_ = lastPingTime_;
        waitingForPong_ = false;
    }
}

void Connection::setPongTimeout(int timeoutSeconds) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pongTimeoutSeconds_ = timeoutSeconds;
}

void Connection::sendLoop() {
    while (active_.load()) {
        std::unique_lock<std::mutex> lock(sendMutex_);
        
        // Check for PING timeout first
        if (waitingForPong_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPingTime_).count();
            if (elapsed >= pongTimeoutSeconds_) {
                lock.unlock();
                notifyError("PONG timeout - connection may be dead");
                break;
            }
        }
        
        // Check if we need to send a PING
        bool shouldSendPing = false;
        if (pingIntervalSeconds_ > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPingTime_).count();
            if (elapsed >= pingIntervalSeconds_ && !waitingForPong_) {
                shouldSendPing = true;
            }
        }
        
        if (shouldSendPing) {
            lock.unlock();
            sendPingFrame(); // Send empty PING
            continue;
        }
        
        // Process all frames in queue
        while (!sendQueue_.empty() && active_.load()) {
            std::vector<uint8_t> frame = sendQueue_.front();
            sendQueue_.pop();
            lock.unlock();
            
            if (!sendRaw(frame)) {
                notifyError("Send failed");
                return;
            }
            
            lock.lock();
        }
        
        // Wait for new data only if queue is empty
        if (sendQueue_.empty()) {
            sendCondition_.wait_for(lock, std::chrono::seconds(1), 
                [this] { return !sendQueue_.empty() || !active_.load(); });
        }
    }
}

void Connection::receiveLoop() {
    std::vector<uint8_t> buffer(Network::RECEIVE_BUFFER_SIZE);
    std::vector<uint8_t> receiveBuffer; // Buffer for incomplete frames
    
    while (active_.load() && socket_ != PLATFORM_INVALID_SOCKET) {
        int result = platform_recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
        
        if (result == PLATFORM_SOCKET_ERROR) {
            int error = platform_socket_error();
            if (error == PLATFORM_EWOULDBLOCK) {
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
            // std::cout << "[DEBUG] Received " << result << " bytes, buffer size now: " << receiveBuffer.size() << std::endl;
            
            // Process complete frames
            size_t processed = 0;
            while (processed < receiveBuffer.size()) {
                size_t frameSize = parseFrameSize(receiveBuffer, processed);
                // std::cout << "[DEBUG] Parsed frame size: " << frameSize << " at offset: " << processed << std::endl;
                
                if (frameSize == 0 || processed + frameSize > receiveBuffer.size()) {
                    // Incomplete frame, wait for more data
                    // std::cout << "[DEBUG] Incomplete frame, waiting for more data" << std::endl;
                    break;
                }
                
                // Extract complete frame
                std::vector<uint8_t> frame(receiveBuffer.begin() + processed, 
                                          receiveBuffer.begin() + processed + frameSize);
                // std::cout << "[DEBUG] Processing frame of size: " << frame.size() << std::endl;
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

bool Connection::sendRaw(const std::vector<uint8_t>& data) {
    if (socket_ == PLATFORM_INVALID_SOCKET) {
        return false;
    }
    
    size_t sent = 0;
    while (sent < data.size()) {
        int result = platform_send(socket_, data.data() + sent, 
                                  static_cast<int>(data.size() - sent), 0);
        if (result == PLATFORM_SOCKET_ERROR) {
            return false;
        }
        sent += result;
    }
    return true;
}

void Connection::handleFrame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) return;
    
    uint8_t firstByte = frame[0];
    uint8_t secondByte = frame[1];
    
    bool fin = (firstByte & Protocol::FIN_FLAG) != 0;
    uint8_t opcode = firstByte & 0x0F;
    bool masked = (secondByte & Protocol::MASK_FLAG) != 0;
    uint8_t payloadLengthByte = secondByte & 0x7F;
    
    size_t headerSize = 2;
    size_t actualPayloadLength = payloadLengthByte;
    
    if (payloadLengthByte == Protocol::PAYLOAD_LENGTH_16BIT) {
        if (frame.size() < 4) return;
        actualPayloadLength = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        headerSize = 4;
    } else if (payloadLengthByte == Protocol::PAYLOAD_LENGTH_64BIT) {
        if (frame.size() < 10) return;
        actualPayloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            actualPayloadLength = (actualPayloadLength << 8) | frame[2 + i];
        }
        headerSize = 10;
    }
    
    if (masked) {
        headerSize += 4;
    }
    
    if (frame.size() < headerSize + actualPayloadLength) return;
    
    std::vector<uint8_t> payload(frame.begin() + headerSize, 
                                frame.begin() + headerSize + actualPayloadLength);
    
    // Unmask payload if masked (optimized batch processing)
    if (masked) {
        std::vector<uint8_t> maskKey(frame.begin() + headerSize - 4, 
                                    frame.begin() + headerSize);
        unmaskPayloadOptimized(payload, maskKey.data());
    }
    
    // Handle control frames first (must be FIN frame)
    if (opcode == Protocol::OPCODE_PING) {
        std::cout << "[PING] Received PING frame, sending PONG" << std::endl;
        
        // Call user callback if set
        { std::lock_guard<std::mutex> lock(callbackMutex_); if (pingCallback_) { try { pingCallback_(payload); } catch (...) {} } }
        
        // Always send PONG response
        sendPongFrame(payload);
        return;
    } else if (opcode == Protocol::OPCODE_PONG) {
        std::cout << "[PONG] Received PONG frame" << std::endl;
        
        // Update PONG timing
            lastPongTime_ = std::chrono::steady_clock::now();
            waitingForPong_ = false;
        
        // Call user callback if set
        { std::lock_guard<std::mutex> lock(callbackMutex_); if (pongCallback_) { try { pongCallback_(payload); } catch (...) {} } }
        return;
    } else if (opcode == Protocol::OPCODE_CLOSE) {
        std::cout << "[CLOSE] Received CLOSE frame" << std::endl;
        handleCloseFrame(payload);
        return;
    }
    
    // Handle fragmented messages with thread safety
    if (opcode == Protocol::OPCODE_CONTINUATION) {
        std::lock_guard<std::mutex> lock(fragmentMutex_);
        if (!hasFragment_) { 
            // std::cout << "[DEBUG] Unexpected continuation frame, no fragment in progress" << std::endl;
            return; 
        }
        // std::cout << "[DEBUG] Continuation frame: payload=" << payload.size() << ", fin=" << fin << std::endl;
        fragmentBuffer_.insert(fragmentBuffer_.end(), payload.begin(), payload.end());
        
        if (fin) {
            // Create Message with optimal storage type for fragment
            Message msg = (fragmentType_ == Message::TEXT) ? 
                Message(std::string(fragmentBuffer_.begin(), fragmentBuffer_.end())) :
                Message(fragmentBuffer_);
            // Notify callback asynchronously to avoid blocking receive loop
            { 
                std::lock_guard<std::mutex> lock(callbackMutex_); 
                if (messageCallback_) { 
                    try { 
                        // Use async to avoid blocking the receive loop
                        messageCallback_(msg);
                    } catch (...) {} 
                } 
            }
            fragmentBuffer_.clear();
            hasFragment_ = false;
        }
    } else if (opcode == Protocol::OPCODE_TEXT || opcode == Protocol::OPCODE_BINARY) {
        Message::Type msgType = (opcode == Protocol::OPCODE_TEXT) ? Message::TEXT : Message::BINARY;
        // std::cout << "[DEBUG] " << (msgType == Message::TEXT ? "TEXT" : "BINARY") 
        //           << " frame: payload=" << payload.size() << ", fin=" << fin << std::endl;
        
        if (fin) {
            // Create Message with optimal storage type - zero copy
            Message msg = (msgType == Message::TEXT) ? 
                Message(std::string(payload.begin(), payload.end())) :
                Message(payload);
            // Notify callback asynchronously to avoid blocking receive loop
            { 
                std::lock_guard<std::mutex> lock(callbackMutex_); 
                if (messageCallback_) { 
                    try { 
                        // Use async to avoid blocking the receive loop
                        messageCallback_(msg);
                    } catch (...) {} 
                } 
            }
        } else {
            // Start of fragmented message - protect with mutex
            std::lock_guard<std::mutex> lock(fragmentMutex_);
            // std::cout << "[DEBUG] Starting fragment: type=" << (msgType == Message::TEXT ? "TEXT" : "BINARY") 
            //           << ", initial size=" << payload.size() << std::endl;
            fragmentBuffer_ = payload;
            fragmentType_ = msgType;
            hasFragment_ = true;
        }
    }
}

size_t Connection::parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset) {
    if (offset + 2 > buffer.size()) return 0;
    
    uint8_t secondByte = buffer[offset + 1];
    uint8_t payloadLength = secondByte & 0x7F;
    size_t headerSize = 2;
    
    if (payloadLength == Protocol::PAYLOAD_LENGTH_16BIT) {
        if (offset + 4 > buffer.size()) return 0;
        headerSize = 4;
    } else if (payloadLength == Protocol::PAYLOAD_LENGTH_64BIT) {
        if (offset + 10 > buffer.size()) return 0;
        headerSize = 10;
    }
    
    bool masked = (secondByte & Protocol::MASK_FLAG) != 0;
    if (masked) {
        headerSize += 4;
    }
    
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

void Connection::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {}
    }
}

void Connection::notifyClose() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (closeCallback_) {
        try {
            closeCallback_();
        } catch (...) {}
    }
}

void Connection::sendPingFrame(const std::vector<uint8_t>& payload) {
    if (socket_ == PLATFORM_INVALID_SOCKET) {
        return;
    }
    
    // Create PING frame
    std::vector<uint8_t> frame;
    
    // First byte: FIN + PING opcode
    frame.push_back(Protocol::FIN_FLAG | Protocol::OPCODE_PING);
    
    // Second byte: payload length
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(Protocol::PAYLOAD_LENGTH_16BIT);
        frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(Protocol::PAYLOAD_LENGTH_64BIT);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
        }
    }
    
    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Send immediately (control frames should not be queued)
    sendRaw(frame);
    
    // Update timing
    lastPingTime_ = std::chrono::steady_clock::now();
    waitingForPong_ = true;
    
    std::cout << "[PING] Sent PING frame with " << payload.size() << " bytes" << std::endl;
}

void Connection::sendPongFrame(const std::vector<uint8_t>& payload) {
    if (socket_ == PLATFORM_INVALID_SOCKET) {
        return;
    }
    
    // Create PONG frame
    std::vector<uint8_t> frame;
    
    // First byte: FIN + PONG opcode
    frame.push_back(Protocol::FIN_FLAG | Protocol::OPCODE_PONG);
    
    // Second byte: payload length
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(Protocol::PAYLOAD_LENGTH_16BIT);
        frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(Protocol::PAYLOAD_LENGTH_64BIT);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
        }
    }
    
    // Payload (echo back the ping payload)
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Send immediately (control frames should not be queued)
    sendRaw(frame);
}

void Connection::handleCloseFrame(const std::vector<uint8_t>& payload) {
    // Parse close code if present
    uint16_t closeCode = Protocol::CLOSE_NORMAL;
    std::string closeReason;
    
    if (payload.size() >= 2) {
        closeCode = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
        if (payload.size() > 2) {
            closeReason = std::string(payload.begin() + 2, payload.end());
        }
    }
    
    // std::cout << "[DEBUG] Close code: " << closeCode << ", reason: " << closeReason << std::endl;
    
    // Send close frame back if we haven't sent one
    if (active_.load()) {
        sendCloseFrame(closeCode, closeReason);
    }
    
    // Close the connection
    close();
}

void Connection::sendCloseFrame(uint16_t code, const std::string& reason) {
    if (socket_ == PLATFORM_INVALID_SOCKET) {
        return;
    }
    
    // Create CLOSE frame
    std::vector<uint8_t> frame;
    
    // First byte: FIN + CLOSE opcode
    frame.push_back(Protocol::FIN_FLAG | Protocol::OPCODE_CLOSE);
    
    // Prepare payload
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    if (!reason.empty()) {
        payload.insert(payload.end(), reason.begin(), reason.end());
    }
    
    // Second byte: payload length
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(Protocol::PAYLOAD_LENGTH_16BIT);
        frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(Protocol::PAYLOAD_LENGTH_64BIT);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
        }
    }
    
    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Send immediately
    sendRaw(frame);
}


// Server implementation
Server::Server() : listenSocket_(PLATFORM_INVALID_SOCKET), running_(false), nextConnectionId_(1) {
    platform_socket_init();
}

Server::~Server() {
    stop();
    platform_socket_cleanup();
}

bool Server::start(int port) {
    if (running_.load()) {
        return false;
    }
    
    // Create listening socket
    listenSocket_ = platform_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == PLATFORM_INVALID_SOCKET) {
        notifyError("Failed to create socket");
        return false;
    }
    
    // Set socket options
    int reuse = 1;
    platform_setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    // Bind to port
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    
    if (platform_bind(listenSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == PLATFORM_SOCKET_ERROR) {
        notifyError("Failed to bind to port " + std::to_string(port));
        platform_close_socket(listenSocket_);
        listenSocket_ = PLATFORM_INVALID_SOCKET;
        return false;
    }
    
    // Start listening
    if (platform_listen(listenSocket_, SOMAXCONN) == PLATFORM_SOCKET_ERROR) {
        notifyError("Failed to listen on socket");
        platform_close_socket(listenSocket_);
        listenSocket_ = PLATFORM_INVALID_SOCKET;
        return false;
    }
    
    running_.store(true);
    acceptThread_ = std::thread(&Server::acceptLoop, this);
    
    return true;
}

void Server::stop() {
    if (running_.exchange(false)) {
        // Close listening socket
        if (listenSocket_ != PLATFORM_INVALID_SOCKET) {
            platform_close_socket(listenSocket_);
            listenSocket_ = PLATFORM_INVALID_SOCKET;
        }
        
        // Wait for accept thread
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        
        // Close all connections
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.clear();
    }
}

void Server::broadcast(const Message& msg) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        if (pair.second->isActive()) {
            pair.second->send(msg);
        }
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
        if (pair.second->isActive()) {
            ids.push_back(pair.first);
        }
    }
    return ids;
}

size_t Server::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    size_t count = 0;
    for (const auto& pair : connections_) {
        if (pair.second->isActive()) {
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
    while (running_.load() && listenSocket_ != PLATFORM_INVALID_SOCKET) {
        sockaddr_in clientAddr;
        platform_socklen_t clientAddrLen = sizeof(clientAddr);
        
        platform_socket_t clientSocket = platform_accept(listenSocket_, (sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket == PLATFORM_INVALID_SOCKET) {
            if (running_.load()) {
                notifyError("Accept failed");
            }
            break;
        }
        
        // Create connection first
        uint64_t connectionId = nextConnectionId_++;
        auto connection = std::make_shared<Connection>(clientSocket, connectionId);
        
        // Perform WebSocket handshake using Connection class
        if (!connection->performServerHandshake(clientSocket)) {
            platform_close_socket(clientSocket);
            continue;
        }
        
        // Set socket options for high performance (same as SelectBasedServer)
        platform_set_tcp_nodelay(clientSocket);
        platform_set_socket_buffer(clientSocket, Network::SOCKET_BUFFER_SIZE);
        
        // Set up connection callbacks
        connection->onClose([this, connectionId]() {
            removeConnection(connectionId);
        });
        
        // Add to connections
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[connectionId] = connection;
        }
        
        // Notify callback
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (connectionCallback_) {
                try {
                    connectionCallback_(connection);
                } catch (...) {}
            }
        }
    }
}

void Server::removeConnection(uint64_t id) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(id);
}

void Server::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {}
    }
}


std::string Server::simpleSHA1(const std::string& input) {
    // Standard SHA1 implementation for WebSocket handshake
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    
    // Add padding
    std::string padded = input;
    padded += static_cast<char>(0x80);
    
    // Pad to 448 bits (56 bytes) mod 512
    while ((padded.length() % SHA1::BLOCK_SIZE) != SHA1::PADDING_MODULO) {
        padded += static_cast<char>(0x00);
    }
    
    // Append original length in bits as 64-bit big-endian
    uint64_t bitLength = input.length() * 8;
    for (int i = 7; i >= 0; --i) {
        padded += static_cast<char>((bitLength >> (i * 8)) & 0xFF);
    }
    
    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < padded.length(); chunk += SHA1::BLOCK_SIZE) {
        uint32_t w[80];
        
        // Break chunk into 16 32-bit big-endian words
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        
        // Extend the 16 32-bit words into 80 32-bit words
        for (int i = 16; i < 80; ++i) {
            w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        
        // Main loop
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    
    // Convert to string (big-endian)
    std::string result;
    for (int i = 0; i < 5; ++i) {
        result += static_cast<char>((h[i] >> 24) & 0xFF);
        result += static_cast<char>((h[i] >> 16) & 0xFF);
        result += static_cast<char>((h[i] >> 8) & 0xFF);
        result += static_cast<char>(h[i] & 0xFF);
    }
    
    return result;
}

std::string Server::base64Encode(const std::string& input) {
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

// ============================================================================
// SelectBasedConnection Implementation - Non-blocking Connection for Select-based Server
// ============================================================================

SelectBasedConnection::SelectBasedConnection(platform_socket_t sock, uint64_t id) 
    : socket_(sock), id_(id), active_(true), hasFragment_(false), fragmentType_(Message::TEXT),
      pingIntervalSeconds_(0), pongTimeoutSeconds_(Network::DEFAULT_PONG_TIMEOUT_SECONDS), waitingForPong_(false) {
    
    // Socket optimization is now handled at Server level for consistency
}

SelectBasedConnection::~SelectBasedConnection() {
    close();
}

bool SelectBasedConnection::sendText(const std::string& text) {
    return send(Message(text));
}

bool SelectBasedConnection::sendBinary(const std::vector<uint8_t>& data) {
    return send(Message(data));
}

bool SelectBasedConnection::send(const Message& msg) {
    if (!active_.load() || socket_ == PLATFORM_INVALID_SOCKET) {
        return false;
    }
    
    try {
        const size_t maxFrameSize = Network::MAX_FRAME_SIZE; // same as Connection
        size_t offset = 0;
        bool isFirstFrame = true;
        
        
        size_t totalFragments = 0;
        
        size_t dataSize = msg.size();
        while (offset < dataSize) {
            size_t frameSize = (maxFrameSize < (dataSize - offset)) ? maxFrameSize : (dataSize - offset);
            bool isLastFrame = (offset + frameSize >= dataSize);
            
            // Create WebSocket frame
            std::vector<uint8_t> frame;
            
            // First byte: FIN + opcode
            uint8_t firstByte = isLastFrame ? 0x80 : 0x00; // FIN flag
            if (isFirstFrame) {
                if (msg.type == Message::TEXT) {
                    firstByte |= 0x01; // TEXT opcode
                } else {
                    firstByte |= 0x02; // BINARY opcode
                }
            } else {
                firstByte |= 0x00; // CONTINUATION opcode
            }
            frame.push_back(firstByte);
            
            // Payload length
            if (frameSize < 126) {
                frame.push_back(static_cast<uint8_t>(frameSize));
            } else if (frameSize < Network::FRAME_SIZE_THRESHOLD_16BIT) {
                frame.push_back(126);
                frame.push_back(static_cast<uint8_t>((frameSize >> 8) & 0xFF));
                frame.push_back(static_cast<uint8_t>(frameSize & 0xFF));
            } else {
                frame.push_back(127);
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
            
            // Add to send queue
            {
                std::lock_guard<std::mutex> lock(sendQueueMutex_);
                sendQueue_.push(frame);
            }
            
            totalFragments++;
            offset += frameSize;
            isFirstFrame = false;
        }
        
        
        return true;
    } catch (...) {
        return false;
    }
}

void SelectBasedConnection::sendPongFrame(const std::vector<uint8_t>& payload) {
    if (!active_.load() || socket_ == PLATFORM_INVALID_SOCKET) {
        return;
    }
    
    // Queue PONG response to avoid interfering with large message sending
    {
        std::lock_guard<std::mutex> lock(pongQueueMutex_);
        pongQueue_.push(payload);
    }
}

void SelectBasedConnection::onMessage(std::function<void(const Message&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    messageCallback_ = callback;
}

void SelectBasedConnection::onError(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = callback;
}

void SelectBasedConnection::onClose(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    closeCallback_ = callback;
}

void SelectBasedConnection::onPing(std::function<void(const std::vector<uint8_t>&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pingCallback_ = callback;
}

void SelectBasedConnection::onPong(std::function<void(const std::vector<uint8_t>&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    pongCallback_ = callback;
}

void SelectBasedConnection::processReceivedData(const char* data, size_t length) {
    if (!active_.load()) {
        return;
    }
    
    // Use the same logic as thread model for consistency and performance
    std::lock_guard<std::mutex> lock(receiveBufferMutex_);
    
    // Add received data to buffer (same as thread model)
    receiveBuffer_.insert(receiveBuffer_.end(), data, data + length);
    
    // Process complete frames using the same logic as thread model
    size_t processed = 0;
    while (processed < receiveBuffer_.size()) {
        size_t frameSize = parseFrameSize(receiveBuffer_, processed);
        
        if (frameSize == 0 || processed + frameSize > receiveBuffer_.size()) {
            // Incomplete frame, wait for more data
            break;
        }
        
        // Extract complete frame
        std::vector<uint8_t> frame(receiveBuffer_.begin() + processed, 
                                  receiveBuffer_.begin() + processed + frameSize);
        
        // Use the same handleFrame method as thread model
        handleFrame(frame);
        
        processed += frameSize;
    }
    
    // Remove processed data (same as thread model)
    if (processed > 0) {
        receiveBuffer_.erase(receiveBuffer_.begin(), receiveBuffer_.begin() + processed);
    }
}

bool SelectBasedConnection::hasDataToSend() const {
    std::lock_guard<std::mutex> sendLock(sendQueueMutex_);
    std::lock_guard<std::mutex> pongLock(pongQueueMutex_);
    return !sendQueue_.empty() || !pongQueue_.empty();
}

bool SelectBasedConnection::hasPongData() const {
    std::lock_guard<std::mutex> lock(pongQueueMutex_);
    return !pongQueue_.empty();
}

std::vector<uint8_t> SelectBasedConnection::getDataToSend() {
    // First check for regular data
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex_);
        if (!sendQueue_.empty()) {
            std::vector<uint8_t> data = sendQueue_.front();
            sendQueue_.pop();
            return data;
        }
    }
    
    // Then check for PONG responses
    {
        std::lock_guard<std::mutex> lock(pongQueueMutex_);
        if (!pongQueue_.empty()) {
            std::vector<uint8_t> payload = pongQueue_.front();
            pongQueue_.pop();
            
            // Create PONG frame
            std::vector<uint8_t> frame;
            
            // First byte: FIN + PONG opcode
            frame.push_back(0x8A); // FIN=1, PONG=0x0A
            
            // Payload length
            size_t payloadLength = payload.size();
            if (payloadLength < 126) {
                frame.push_back(static_cast<uint8_t>(payloadLength));
            } else if (payloadLength < 65536) {
                frame.push_back(126);
                frame.push_back(static_cast<uint8_t>((payloadLength >> 8) & 0xFF));
                frame.push_back(static_cast<uint8_t>(payloadLength & 0xFF));
            } else {
                frame.push_back(127);
                for (int i = 7; i >= 0; --i) {
                    frame.push_back(static_cast<uint8_t>((payloadLength >> (i * 8)) & 0xFF));
                }
            }
            
            // Payload data
            frame.insert(frame.end(), payload.begin(), payload.end());
            
            return frame;
        }
    }
    
    return std::vector<uint8_t>();
}

void SelectBasedConnection::putDataBackToQueue(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(sendQueueMutex_);
    // Put data at front of queue for next attempt
    std::queue<std::vector<uint8_t>> tempQueue;
    tempQueue.push(data);
    while (!sendQueue_.empty()) {
        tempQueue.push(sendQueue_.front());
        sendQueue_.pop();
    }
    sendQueue_ = std::move(tempQueue);
}

void SelectBasedConnection::putRemainingDataBack(const std::vector<uint8_t>& remaining) {
    std::lock_guard<std::mutex> lock(sendQueueMutex_);
    // Put remaining data at front of queue to continue sending the same frame
    // This maintains frame order while allowing partial send retry
    std::queue<std::vector<uint8_t>> tempQueue;
    tempQueue.push(remaining);
    while (!sendQueue_.empty()) {
        tempQueue.push(sendQueue_.front());
        sendQueue_.pop();
    }
    sendQueue_ = std::move(tempQueue);
}

void SelectBasedConnection::close() {
    if (active_.exchange(false)) {
        if (socket_ != PLATFORM_INVALID_SOCKET) {
            platform_close_socket(socket_);
            socket_ = PLATFORM_INVALID_SOCKET;
        }
        notifyClose();
    }
}

bool SelectBasedConnection::performServerHandshake(platform_socket_t clientSocket) {
    // Use the same handshake implementation as Connection class
    // Read HTTP request
    std::string request;
    char buffer[Network::HANDSHAKE_BUFFER_SIZE];
    
    while (true) {
        int result = platform_recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (result <= 0) {
            return false;
        }
        
        buffer[result] = '\0';
        request += buffer;
        
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    
    // Parse Sec-WebSocket-Key
    std::string key;
    std::string keyHeader = std::string(Handshake::WEBSOCKET_KEY_HEADER) + ": ";
    size_t keyStart = request.find(keyHeader);
    if (keyStart != std::string::npos) {
        keyStart += keyHeader.length();
        size_t keyEnd = request.find("\r\n", keyStart);
        if (keyEnd != std::string::npos) {
            key = request.substr(keyStart, keyEnd - keyStart);
        }
    }
    
    if (key.empty()) {
        return false;
    }
    
    // Generate Sec-WebSocket-Accept
    std::string acceptKey = key + Handshake::WEBSOCKET_MAGIC_STRING;
    std::string acceptHash = simpleSHA1(acceptKey);
    std::string acceptValue = base64Encode(acceptHash);
    
    // Send HTTP response
    std::string response = 
        std::string(Handshake::HTTP_SWITCHING_PROTOCOLS) + "\r\n"
        + std::string(Handshake::HTTP_UPGRADE_HEADER) + "\r\n"
        + std::string(Handshake::HTTP_CONNECTION_HEADER) + "\r\n"
        "Sec-WebSocket-Accept: " + acceptValue + "\r\n"
        "\r\n";
    
    int sent = platform_send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
    return sent > 0;
}

std::string SelectBasedConnection::simpleSHA1(const std::string& input) {
    // Standard SHA1 implementation for WebSocket handshake
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    
    // Add padding
    std::string padded = input;
    padded += static_cast<char>(0x80);
    
    // Pad to 448 bits (56 bytes) mod 512
    while ((padded.length() % SHA1::BLOCK_SIZE) != SHA1::PADDING_MODULO) {
        padded += static_cast<char>(0x00);
    }
    
    // Append original length in bits as 64-bit big-endian
    uint64_t bitLength = input.length() * 8;
    for (int i = 7; i >= 0; --i) {
        padded += static_cast<char>((bitLength >> (i * 8)) & 0xFF);
    }
    
    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < padded.length(); chunk += SHA1::BLOCK_SIZE) {
        uint32_t w[80];
        
        // Break chunk into 16 32-bit big-endian words
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        
        // Extend the 16 32-bit words into 80 32-bit words
        for (int i = 16; i < 80; ++i) {
            w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        
        // Main loop
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    
    // Convert to string (big-endian)
    std::string result;
    for (int i = 0; i < 5; ++i) {
        result += static_cast<char>((h[i] >> 24) & 0xFF);
        result += static_cast<char>((h[i] >> 16) & 0xFF);
        result += static_cast<char>((h[i] >> 8) & 0xFF);
        result += static_cast<char>(h[i] & 0xFF);
    }
    
    return result;
}

std::string SelectBasedConnection::base64Encode(const std::string& input) {
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

void SelectBasedConnection::parseFrameFromBuffer() {
    // This method is no longer used - we use the same logic as thread model
    // Kept for compatibility but not called
}

// Frame processing methods (copied from Connection for performance)
size_t SelectBasedConnection::parseFrameSize(const std::vector<uint8_t>& buffer, size_t offset) {
    if (offset + 2 > buffer.size()) return 0;
    
    uint8_t secondByte = buffer[offset + 1];
    uint8_t payloadLength = secondByte & 0x7F;
    size_t headerSize = 2;
    
    if (payloadLength == Protocol::PAYLOAD_LENGTH_16BIT) {
        if (offset + 4 > buffer.size()) return 0;
        headerSize = 4;
    } else if (payloadLength == Protocol::PAYLOAD_LENGTH_64BIT) {
        if (offset + 10 > buffer.size()) return 0;
        headerSize = 10;
    }
    
    bool masked = (secondByte & 0x80) != 0; // MASK_FLAG
    if (masked) {
        headerSize += 4;
    }
    
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

void SelectBasedConnection::handleFrame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) return;
    
    uint8_t firstByte = frame[0];
    uint8_t secondByte = frame[1];
    
    bool fin = (firstByte & 0x80) != 0;
    uint8_t opcode = firstByte & 0x0F;
    bool masked = (secondByte & 0x80) != 0;
    uint64_t payloadLength = secondByte & 0x7F;
    
    size_t headerSize = 2;
    
    // Parse payload length
    if (payloadLength == 126) {
        if (frame.size() < 4) return;
        payloadLength = (static_cast<uint64_t>(frame[2]) << 8) | frame[3];
        headerSize = 4;
    } else if (payloadLength == 127) {
        if (frame.size() < 10) return;
        payloadLength = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) | frame[2 + i];
        }
        headerSize = 10;
    }
    
    // Parse mask (same as thread model - no duplicate parsing)
    if (masked) {
        if (frame.size() < headerSize + 4) return;
        headerSize += 4;
    }
    
    // Extract payload as vector<uint8_t> for optimal performance
    if (frame.size() < headerSize + payloadLength) return;
    
    std::vector<uint8_t> payload(frame.begin() + headerSize, 
                       frame.begin() + headerSize + payloadLength);
    
    // Unmask payload if masked (optimized batch processing)
    if (masked) {
        std::vector<uint8_t> maskKey(frame.begin() + headerSize - 4, 
                                    frame.begin() + headerSize);
        unmaskPayloadOptimized(payload, maskKey.data());
    }
    
    // Handle frame based on opcode
    if (opcode == 0x08) { // CLOSE
        close();
        return;
    } else if (opcode == 0x09) { // PING
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (pingCallback_) {
                try {
                    pingCallback_(payload);
                } catch (...) {}
            }
        }
    } else if (opcode == 0x0A) { // PONG
        // Reset PING timeout
        waitingForPong_.store(false);
        
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            if (pongCallback_) {
                try {
                    pongCallback_(payload);
                } catch (...) {}
            }
        }
    } else if (opcode == 0x00) { // CONTINUATION
        // Handle fragmented messages
        if (!hasFragment_) {
            // Unexpected continuation frame, no fragment in progress
            return;
        }
        
        fragmentBuffer_.insert(fragmentBuffer_.end(), payload.begin(), payload.end());
        
        if (fin) {
            // Fragment complete - create Message with optimal storage type
            Message msg = (fragmentType_ == Message::TEXT) ? 
                Message(std::string(fragmentBuffer_.begin(), fragmentBuffer_.end())) :
                Message(std::move(fragmentBuffer_));
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (messageCallback_) {
                    try {
                        // Use async to avoid blocking the select event loop
                        messageCallback_(msg);
                    } catch (...) {}
                }
            }
            fragmentBuffer_.clear();
            hasFragment_ = false;
        }
    } else if (opcode == 0x01 || opcode == 0x02) { // TEXT or BINARY
        Message::Type msgType = (opcode == 0x01) ? Message::TEXT : Message::BINARY;
        
        if (fin) {
            // Complete message - create Message with optimal storage type
            Message msg = (msgType == Message::TEXT) ? 
                Message(std::string(payload.begin(), payload.end())) :  // convert to string for TEXT
                Message(std::move(payload));  // move vector for BINARY
            {
                std::lock_guard<std::mutex> lock(callbackMutex_);
                if (messageCallback_) {
                    try {
                        // Use async to avoid blocking the select event loop
                        messageCallback_(msg);
                    } catch (...) {}
                }
            }
        } else {
            // Start of fragmented message (payload is already vector<uint8_t>)
            fragmentBuffer_ = std::move(payload);
            fragmentType_ = msgType;
            hasFragment_ = true;
        }
    }
}

void SelectBasedConnection::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {}
    }
}

void SelectBasedConnection::notifyClose() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (closeCallback_) {
        try {
            closeCallback_();
        } catch (...) {}
    }
}

// ============================================================================
// SelectBasedServer Implementation - High Performance Select-based Server
// ============================================================================

SelectBasedServer::SelectBasedServer() 
    : listenSocket_(PLATFORM_INVALID_SOCKET), running_(false), nextConnectionId_(1) {
}

SelectBasedServer::~SelectBasedServer() {
    stop();
}

bool SelectBasedServer::start(int port) {
    if (running_.load()) {
        return false;
    }
    
    // Initialize platform socket
    if (platform_socket_init() != 0) {
        notifyError("Platform socket initialization failed");
        return false;
    }
    
    // Create listening socket
    listenSocket_ = platform_socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ == PLATFORM_INVALID_SOCKET) {
        notifyError("Failed to create listening socket");
        return false;
    }
    
    // Set socket options for high performance
    platform_set_tcp_nodelay(listenSocket_);
        platform_set_socket_buffer(listenSocket_, Network::SOCKET_BUFFER_SIZE);
    
    // Set non-blocking mode for select
    if (platform_set_nonblocking(listenSocket_) != 0) {
        notifyError("Failed to set non-blocking mode");
        platform_close_socket(listenSocket_);
        return false;
    }
    
    // Bind to port
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    
    if (platform_bind(listenSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == PLATFORM_SOCKET_ERROR) {
        notifyError("Failed to bind to port " + std::to_string(port));
        platform_close_socket(listenSocket_);
        return false;
    }
    
    // Start listening
    if (platform_listen(listenSocket_, SOMAXCONN) == PLATFORM_SOCKET_ERROR) {
        notifyError("Failed to start listening");
        platform_close_socket(listenSocket_);
        return false;
    }
    
    running_.store(true);
    eventLoopThread_ = std::thread(&SelectBasedServer::eventLoop, this);
    
    return true;
}

void SelectBasedServer::stop() {
    if (running_.exchange(false)) {
        // Close listening socket
        if (listenSocket_ != PLATFORM_INVALID_SOCKET) {
            platform_close_socket(listenSocket_);
            listenSocket_ = PLATFORM_INVALID_SOCKET;
        }
        
        
        // Wait for event loop thread
        if (eventLoopThread_.joinable()) {
            eventLoopThread_.join();
        }
        
        // Close all connections
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.clear();
    }
}

void SelectBasedServer::broadcast(const Message& msg) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        pair.second->send(msg);
    }
}

void SelectBasedServer::broadcastText(const std::string& text) {
    broadcast(Message(text));
}

void SelectBasedServer::broadcastBinary(const std::vector<uint8_t>& data) {
    broadcast(Message(data));
}

std::vector<uint64_t> SelectBasedServer::getConnections() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::vector<uint64_t> result;
    for (const auto& pair : connections_) {
        result.push_back(pair.first);
    }
    return result;
}

size_t SelectBasedServer::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

void SelectBasedServer::onConnection(std::function<void(std::shared_ptr<SelectBasedConnection>)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    connectionCallback_ = callback;
}

void SelectBasedServer::onError(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    errorCallback_ = callback;
}

void SelectBasedServer::eventLoop() {
    while (running_.load()) {
        fd_set readfds, writefds;
        int max_fd = 0;
        
        // Setup select sets
            setupSelectSets(readfds, writefds, max_fd);
        
        if (max_fd == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // Set timeout for select - adaptive based on activity
        struct timeval timeout;
        timeout.tv_sec = 0;
        // Use shorter timeout for better responsiveness
        timeout.tv_usec = Network::SELECT_TIMEOUT_USEC;
        
        // Wait for events
        int result = platform_select(max_fd + 1, &readfds, &writefds, NULL, &timeout);
        
        if (result == PLATFORM_SOCKET_ERROR) {
            if (running_.load()) {
                notifyError("Select failed");
            }
            break;
        } else if (result > 0) {
            // Handle events
            handleSelectEvents(readfds, writefds);
        }
        
        // Check PING timeouts for all connections
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            for (auto& pair : connections_) {
                if (pair.second->isActive()) {
                    pair.second->checkPingTimeout();
                }
            }
        }
        
        // Timeout is normal, continue loop
    }
}

void SelectBasedServer::setupSelectSets(fd_set& readfds, fd_set& writefds, int& max_fd) {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    max_fd = 0;
    
    // Add listening socket
    if (listenSocket_ != PLATFORM_INVALID_SOCKET) {
        FD_SET(listenSocket_, &readfds);
        max_fd = (max_fd > static_cast<int>(listenSocket_)) ? max_fd : static_cast<int>(listenSocket_);
    }
    
    // Add all connection sockets
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (const auto& pair : connections_) {
            if (pair.second->isActive()) {
            platform_socket_t sock = pair.second->getSocket();
        if (sock != PLATFORM_INVALID_SOCKET) {
            FD_SET(sock, &readfds);
                // Only monitor write events when there's data to send
                if (pair.second->hasDataToSend()) {
            FD_SET(sock, &writefds);
                }
            max_fd = (max_fd > static_cast<int>(sock)) ? max_fd : static_cast<int>(sock);
            }
        }
    }
}

void SelectBasedServer::handleSelectEvents(const fd_set& readfds, const fd_set& writefds) {
    // Check for new connections
    if (listenSocket_ != PLATFORM_INVALID_SOCKET && FD_ISSET(listenSocket_, &readfds)) {
        handleNewConnection();
    }
    
    // Collect connections to remove (to avoid deadlock)
    std::vector<uint64_t> connectionsToRemove;
    
    // Check for data on existing connections
    // Copy active connections to reduce lock holding time
    std::vector<std::shared_ptr<SelectBasedConnection>> activeConnections;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (it->second->isActive()) {
                activeConnections.push_back(it->second);
                ++it;
            } else {
                // Remove inactive connections
                it = connections_.erase(it);
            }
        }
    }
    
    // Process connections without holding the lock
    // Phase 1: Handle all read events first (receive priority)
    for (auto& conn : activeConnections) {
        platform_socket_t sock = conn->getSocket();
        if (sock != PLATFORM_INVALID_SOCKET) {
            // Handle read events only
            if (FD_ISSET(sock, &readfds)) {
                handleConnectionData(conn, connectionsToRemove);
            }
        }
    }
    
    // Phase 2: Handle all write events after receiving (send after receive)
    for (auto& conn : activeConnections) {
        platform_socket_t sock = conn->getSocket();
        if (sock != PLATFORM_INVALID_SOCKET) {
            // Handle write events only
            if (FD_ISSET(sock, &writefds)) {
                handleConnectionWrite(conn, connectionsToRemove);
            }
        }
    }
    
    // Remove connections that were marked for removal (outside of lock)
    for (uint64_t id : connectionsToRemove) {
        removeConnection(id);
    }
}

void SelectBasedServer::handleNewConnection() {
    sockaddr_in clientAddr;
    platform_socklen_t clientAddrLen = sizeof(clientAddr);
    
    platform_socket_t clientSocket = platform_accept(listenSocket_, (sockaddr*)&clientAddr, &clientAddrLen);
    
    if (clientSocket == PLATFORM_INVALID_SOCKET) {
        if (running_.load()) {
            notifyError("Accept failed");
        }
        return;
    }
    
    // Ensure client socket is in blocking mode for handshake
    platform_set_blocking(clientSocket);
    
    // Create connection first
    uint64_t connectionId = nextConnectionId_++;
    auto connection = std::make_shared<SelectBasedConnection>(clientSocket, connectionId);
    
    // Perform WebSocket handshake using Connection class
    if (!connection->performServerHandshake(clientSocket)) {
        platform_close_socket(clientSocket);
        return;
    }
    
    // Set non-blocking mode for the new connection
    platform_set_nonblocking(clientSocket);
    platform_set_tcp_nodelay(clientSocket);
        platform_set_socket_buffer(clientSocket, Network::SOCKET_BUFFER_SIZE);
    
    // Set up connection callbacks
    connection->onClose([this, connectionId]() {
        // Don't remove connection here to avoid deadlock
        // The connection will be cleaned up by handleSelectEvents loop
        std::cout << "Connection " << connectionId << " marked for cleanup" << std::endl;
    });
    
    // Add to connections
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[connectionId] = connection;
    }
    
    // Set PING interval for connection health monitoring
    connection->setPingInterval(60); // Send PING every 60 seconds
    connection->setPongTimeout(120);  // Wait 120 seconds for PONG response
    
    // Notify callback
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (connectionCallback_) {
            try {
                connectionCallback_(connection);
            } catch (...) {}
        }
    }
}

void SelectBasedServer::handleConnectionData(std::shared_ptr<SelectBasedConnection> conn, std::vector<uint64_t>& connectionsToRemove) {
    // Handle non-blocking data reception for select-based server
    // This method is called when select indicates data is available on the socket
    // Use the same continuous receive logic as thread model for better performance
    
    std::vector<char> buffer(Network::RECEIVE_BUFFER_SIZE);
    
    // Continuous receive loop like thread model - process immediately instead of batching
    while (true) {
        int bytesRead = platform_recv(conn->getSocket(), buffer.data(), buffer.size(), 0);
        
        if (bytesRead > 0) {
            // Process received data immediately (like thread model)
            conn->processReceivedData(buffer.data(), bytesRead);
            
            // Continue reading if there might be more data
            // This matches thread model's continuous receive behavior
        } else if (bytesRead == 0) {
            // Connection closed by client
            conn->close();
            connectionsToRemove.push_back(conn->getId());
            break;
        } else {
            // Error or would block
            if (!platform_would_block()) {
                // Real error
                conn->close();
                connectionsToRemove.push_back(conn->getId());
            }
            // If would block, no more data available - exit loop
            break;
        }
    }
}

void SelectBasedServer::handleConnectionWrite(std::shared_ptr<SelectBasedConnection> conn, std::vector<uint64_t>& connectionsToRemove) {
    // Handle non-blocking data sending for select-based server
    // This method is called when select indicates socket is ready for writing
    
    // Only process if there's actually data to send
    if (!conn->hasDataToSend()) {
        return;
    }
    
    size_t totalSent = 0;
    size_t framesSent = 0;
    
    // Process frames with per-frame retry mechanism
    int maxRetries = 3; // Maximum retries per frame for partial sends
    int maxFramesPerBatch = Network::MAX_FRAMES_PER_BATCH;
    int framesProcessed = 0;
    
    // Process frames in batches, ensuring frame order and completeness
    while (conn->hasDataToSend() && framesProcessed < maxFramesPerBatch) {
        std::vector<uint8_t> data = conn->getDataToSend();
        if (data.empty()) {
            break;
        }
        
        // Retry mechanism for this specific frame
        int frameRetryCount = 0;
        bool frameSent = false;
        
        while (frameRetryCount < maxRetries && !frameSent) {
            int bytesSent = platform_send(conn->getSocket(), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
        
        if (bytesSent < 0) {
            if (!platform_would_block()) {
                // Real error
                conn->close();
                connectionsToRemove.push_back(conn->getId());
                    return; // Exit immediately on real error
                }
                // Would block - retry this frame
                frameRetryCount++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            } else if (bytesSent < static_cast<int>(data.size())) {
                // Partial send - update data to remaining part and retry
                data.erase(data.begin(), data.begin() + bytesSent);
                
                // Add minimal delay before retry to allow socket buffer to clear
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                frameRetryCount++;
                continue; // Retry sending the remaining data
            } else {
                // Complete send successful
                totalSent += bytesSent;
                framesSent++;
                framesProcessed++;
                frameSent = true;
            }
        }
        
        // If frame couldn't be sent after max retries, put it back and exit
        if (!frameSent) {
            conn->putRemainingDataBack(data);
            std::cout << "[WARNING] Frame retry limit reached for connection " << conn->getId() 
                     << ", will retry on next write event" << std::endl;
            break; // Exit to allow other connections to be processed
        }
    }
}

void SelectBasedServer::removeConnection(uint64_t id) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(id);
}


void SelectBasedServer::notifyError(const std::string& error) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (errorCallback_) {
        try {
            errorCallback_(error);
        } catch (...) {}
    }
}


std::string SelectBasedServer::simpleSHA1(const std::string& input) {
    // Copy the SHA1 implementation from Server class
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    
    // Add padding
    std::string padded = input;
    padded += static_cast<char>(0x80);
    
    // Pad to 448 bits (56 bytes) mod 512
    while ((padded.length() % SHA1::BLOCK_SIZE) != SHA1::PADDING_MODULO) {
        padded += static_cast<char>(0x00);
    }
    
    // Append original length in bits as 64-bit big-endian
    uint64_t bitLength = input.length() * 8;
    for (int i = 7; i >= 0; --i) {
        padded += static_cast<char>((bitLength >> (i * 8)) & 0xFF);
    }
    
    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < padded.length(); chunk += SHA1::BLOCK_SIZE) {
        uint32_t w[80];
        
        // Break chunk into 16 32-bit big-endian words
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        
        // Extend the 16 32-bit words into 80 32-bit words
        for (int i = 16; i < 80; ++i) {
            w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (w[i] << 1) | (w[i] >> 31);
        }
        
        // Initialize hash value for this chunk
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        
        // Main loop
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = temp;
        }
        
        // Add this chunk's hash to result
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    
    // Produce the final hash value as a 160-bit number
    std::string result;
    for (int i = 0; i < 5; ++i) {
        for (int j = 3; j >= 0; --j) {
            result += static_cast<char>((h[i] >> (j * 8)) & 0xFF);
        }
    }
    
    return result;
}

std::string SelectBasedServer::base64Encode(const std::string& input) {
    // Copy the Base64 implementation from Server class
    const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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


// PING/PONG methods for SelectBasedConnection
void SelectBasedConnection::setPingInterval(int seconds) {
    pingIntervalSeconds_ = seconds;
}

void SelectBasedConnection::setPongTimeout(int seconds) {
    pongTimeoutSeconds_ = seconds;
}

void SelectBasedConnection::sendPingFrame(const std::vector<uint8_t>& payload) {
    if (!active_.load() || socket_ == PLATFORM_INVALID_SOCKET) {
        return;
    }
    
    // Create PING frame
    std::vector<uint8_t> frame;
    frame.push_back(0x89); // FIN=1, opcode=9 (PING)
    
    // Payload length
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((payload.size() >> (i * 8)) & 0xFF));
        }
    }
    
    // Add payload
    frame.insert(frame.end(), payload.begin(), payload.end());
    
    // Send frame
    {
        std::lock_guard<std::mutex> lock(sendQueueMutex_);
        sendQueue_.push(frame);
    }
    
    waitingForPong_.store(true);
    lastPingTime_ = std::chrono::steady_clock::now();
    
    std::cout << "[PING] Sent PING frame with " << payload.size() << " bytes" << std::endl;
}

void SelectBasedConnection::checkPingTimeout() {
    if (waitingForPong_.load()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPingTime_).count();
        if (elapsed >= pongTimeoutSeconds_) {
            std::cout << "[PING] PONG timeout for connection " << id_ << " after " << elapsed << " seconds" << std::endl;
            notifyError("PONG timeout - connection may be dead");
            close(); // Close connection on PONG timeout
            return;
        }
    }
    
    // Check if we need to send a PING
    if (pingIntervalSeconds_ > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastPingTime_).count();
        if (elapsed >= pingIntervalSeconds_ && !waitingForPong_.load()) {
            std::cout << "[PING] Sending PING to connection " << id_ << " after " << elapsed << " seconds" << std::endl;
            sendPingFrame(); // Send empty PING
        }
    }
}

} // namespace WebSocketSimple
