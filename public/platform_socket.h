#pragma once

// Platform-specific socket abstraction layer
// Based on Lei Ge's philosophy: eliminate special cases, make platform differences normal

#ifdef _WIN32
    // Windows implementation
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    
    typedef SOCKET platform_socket_t;
    typedef int platform_socklen_t;
    
    #define PLATFORM_INVALID_SOCKET INVALID_SOCKET
    #define PLATFORM_SOCKET_ERROR SOCKET_ERROR
    
    inline int platform_socket_init() {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData);
    }
    
    inline void platform_socket_cleanup() {
        WSACleanup();
    }
    
    inline int platform_socket_error() {
        return WSAGetLastError();
    }
    
    inline int platform_close_socket(platform_socket_t sock) {
        return closesocket(sock);
    }
    
    inline int platform_connect(platform_socket_t sock, const struct sockaddr* addr, platform_socklen_t addrlen) {
        return ::connect(sock, addr, addrlen);
    }
    
    inline int platform_send(platform_socket_t sock, const void* buf, int len, int flags) {
        return ::send(sock, (const char*)buf, len, flags);
    }
    
    inline int platform_recv(platform_socket_t sock, void* buf, int len, int flags) {
        return ::recv(sock, (char*)buf, len, flags);
    }
    
    inline int platform_accept(platform_socket_t sock, struct sockaddr* addr, platform_socklen_t* addrlen) {
        return ::accept(sock, addr, addrlen);
    }
    
    inline int platform_bind(platform_socket_t sock, const struct sockaddr* addr, platform_socklen_t addrlen) {
        return ::bind(sock, addr, addrlen);
    }
    
    inline int platform_listen(platform_socket_t sock, int backlog) {
        return ::listen(sock, backlog);
    }
    
    inline platform_socket_t platform_socket(int domain, int type, int protocol) {
        return ::socket(domain, type, protocol);
    }
    
    inline int platform_setsockopt(platform_socket_t sock, int level, int optname, const void* optval, platform_socklen_t optlen) {
        return ::setsockopt(sock, level, optname, (const char*)optval, optlen);
    }
    
    #define PLATFORM_EWOULDBLOCK WSAEWOULDBLOCK

#else
    // Linux/Mac implementation
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    
    typedef int platform_socket_t;
    typedef socklen_t platform_socklen_t;
    
    #define PLATFORM_INVALID_SOCKET -1
    #define PLATFORM_SOCKET_ERROR -1
    
    inline int platform_socket_init() {
        return 0; // No initialization needed on Unix
    }
    
    inline void platform_socket_cleanup() {
        // No cleanup needed on Unix
    }
    
    inline int platform_socket_error() {
        return errno;
    }
    
    inline int platform_close_socket(platform_socket_t sock) {
        return close(sock);
    }
    
    inline int platform_connect(platform_socket_t sock, const struct sockaddr* addr, platform_socklen_t addrlen) {
        return ::connect(sock, addr, addrlen);
    }
    
    inline int platform_send(platform_socket_t sock, const void* buf, int len, int flags) {
        return ::send(sock, buf, len, flags);
    }
    
    inline int platform_recv(platform_socket_t sock, void* buf, int len, int flags) {
        return ::recv(sock, buf, len, flags);
    }
    
    inline int platform_accept(platform_socket_t sock, struct sockaddr* addr, platform_socklen_t* addrlen) {
        return ::accept(sock, addr, addrlen);
    }
    
    inline int platform_bind(platform_socket_t sock, const struct sockaddr* addr, platform_socklen_t addrlen) {
        return ::bind(sock, addr, addrlen);
    }
    
    inline int platform_listen(platform_socket_t sock, int backlog) {
        return ::listen(sock, backlog);
    }
    
    inline platform_socket_t platform_socket(int domain, int type, int protocol) {
        return ::socket(domain, type, protocol);
    }
    
    inline int platform_setsockopt(platform_socket_t sock, int level, int optname, const void* optval, platform_socklen_t optlen) {
        return ::setsockopt(sock, level, optname, optval, optlen);
    }
    
    #define PLATFORM_EWOULDBLOCK EWOULDBLOCK

#endif

// Common socket options
inline int platform_set_nonblocking(platform_socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline int platform_set_blocking(platform_socket_t sock) {
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

inline int platform_set_tcp_nodelay(platform_socket_t sock) {
    int nodelay = 1;
    return platform_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

inline int platform_set_socket_buffer(platform_socket_t sock, int bufferSize) {
    int result = platform_setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
    if (result == 0) {
        result = platform_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
    }
    return result;
}

// Select-based I/O multiplexing
inline int platform_select(int nfds, fd_set *readfds, fd_set *writefds, 
                          fd_set *exceptfds, struct timeval *timeout) {
#ifdef _WIN32
    return select(nfds, readfds, writefds, exceptfds, timeout);
#else
    return select(nfds, readfds, writefds, exceptfds, timeout);
#endif
}


// Get socket error for non-blocking operations
inline bool platform_would_block() {
#ifdef _WIN32
    int error = WSAGetLastError();
    return (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS);
#else
    return (errno == EWOULDBLOCK || errno == EAGAIN);
#endif
}
