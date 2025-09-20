#pragma once

#include <cstdint>

namespace WebSocketSimple {

// WebSocket Protocol Constants (RFC 6455)
namespace Protocol {
    // Frame flags
    constexpr uint8_t FIN_FLAG = 0x80;
    constexpr uint8_t MASK_FLAG = 0x80;
    
    // Opcodes
    constexpr uint8_t OPCODE_CONTINUATION = 0x00;
    constexpr uint8_t OPCODE_TEXT = 0x01;
    constexpr uint8_t OPCODE_BINARY = 0x02;
    constexpr uint8_t OPCODE_CLOSE = 0x08;
    constexpr uint8_t OPCODE_PING = 0x09;
    constexpr uint8_t OPCODE_PONG = 0x0A;

    // Payload length indicators
    constexpr uint8_t PAYLOAD_LENGTH_16BIT = 126;
    constexpr uint8_t PAYLOAD_LENGTH_64BIT = 127;

    // Close codes
    constexpr uint16_t CLOSE_NORMAL = 1000;
    constexpr uint16_t CLOSE_GOING_AWAY = 1001;
    constexpr uint16_t CLOSE_PROTOCOL_ERROR = 1002;
    constexpr uint16_t CLOSE_UNSUPPORTED_DATA = 1003;
    constexpr uint16_t CLOSE_NO_STATUS_RECEIVED = 1005;
    constexpr uint16_t CLOSE_ABNORMAL_CLOSURE = 1006;
    constexpr uint16_t CLOSE_INVALID_FRAME_PAYLOAD_DATA = 1007;
    constexpr uint16_t CLOSE_POLICY_VIOLATION = 1008;
    constexpr uint16_t CLOSE_MESSAGE_TOO_BIG = 1009;
    constexpr uint16_t CLOSE_MANDATORY_EXTENSION = 1010;
    constexpr uint16_t CLOSE_INTERNAL_ERROR = 1011;
    constexpr uint16_t CLOSE_SERVICE_RESTART = 1012;
    constexpr uint16_t CLOSE_TRY_AGAIN_LATER = 1013;
    constexpr uint16_t CLOSE_BAD_GATEWAY = 1014;
    constexpr uint16_t CLOSE_TLS_HANDSHAKE = 1015;
}

// Network Configuration Constants
namespace Network {
    // Buffer sizes
    constexpr size_t SOCKET_BUFFER_SIZE = 1024 * 1024;        // 1MB socket buffers
    constexpr size_t RECEIVE_BUFFER_SIZE = 1024 * 1024;       // 1MB receive buffer
    constexpr size_t HANDSHAKE_BUFFER_SIZE = 1024;            // 1KB handshake buffer
    
    // Frame sizes
    constexpr size_t MAX_FRAME_SIZE = 64 * 1024;              // 64KB per frame (optimized)
    constexpr size_t FRAME_SIZE_THRESHOLD_16BIT = 65536;      // 64KB threshold for 16-bit length
    
    // Batch processing
    constexpr int MAX_FRAMES_PER_BATCH = 1000;                // Process up to 1000 frames per batch
    
    // Timeouts
    constexpr int SELECT_TIMEOUT_USEC = 1000;                 // 1ms timeout for select()
    constexpr int DEFAULT_PONG_TIMEOUT_SECONDS = 30;          // 30 seconds pong timeout
    constexpr int DEFAULT_PING_INTERVAL_SECONDS = 0;          // 0 = disabled by default
}

// SHA-1 Constants
namespace SHA1 {
    constexpr size_t BLOCK_SIZE = 64;                         // SHA-1 block size
    constexpr size_t PADDING_MODULO = 56;                     // Padding modulo for SHA-1
    constexpr size_t HASH_SIZE = 20;                          // SHA-1 hash size in bytes
}

// WebSocket Handshake Constants
namespace Handshake {
    // HTTP Headers
    constexpr const char* WEBSOCKET_KEY_HEADER = "Sec-WebSocket-Key";
    constexpr const char* WEBSOCKET_ACCEPT_HEADER = "Sec-WebSocket-Accept";
    constexpr const char* WEBSOCKET_VERSION_HEADER = "Sec-WebSocket-Version";
    constexpr const char* WEBSOCKET_PROTOCOL_HEADER = "Sec-WebSocket-Protocol";
    constexpr const char* WEBSOCKET_EXTENSIONS_HEADER = "Sec-WebSocket-Extensions";
    
    // HTTP Request/Response Templates
    constexpr const char* HTTP_UPGRADE_HEADER = "Upgrade: websocket";
    constexpr const char* HTTP_CONNECTION_HEADER = "Connection: Upgrade";
    constexpr const char* HTTP_SWITCHING_PROTOCOLS = "HTTP/1.1 101 Switching Protocols";
    
    // WebSocket Protocol Constants
    constexpr const char* WEBSOCKET_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    constexpr const char* WEBSOCKET_VERSION = "13";
    
    // Request Path
    constexpr const char* DEFAULT_PATH = "/";
}

// Performance Constants
namespace Performance {
    // Socket options
    constexpr int TCP_NODELAY_ENABLE = 1;                     // Enable TCP_NODELAY
    constexpr int SO_REUSEADDR_ENABLE = 1;                    // Enable SO_REUSEADDR
    
    // Connection limits
    constexpr size_t MAX_CONNECTIONS = 1000;                  // Maximum concurrent connections
    constexpr size_t MAX_MESSAGE_SIZE = 100 * 1024 * 1024;    // 100MB max message size
}

} // namespace WebSocketSimple
