#include "server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tinykv {

Server::Server(ServerConfig config)
    : config_(std::move(config)) {}

Server::~Server() {
    stop();
}

int Server::init() {
    // 1. Create the listening socket endpoint.
    // AF_INET: IPv4 Internet protocols.
    // SOCK_STREAM: Provides sequenced, reliable, two-way, connection-based byte streams (TCP).
    // Protocol 0: Automatically selects default protocol for AF_INET + SOCK_STREAM (IPPROTO_TCP).
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "[ERROR] socket() failed: %s (errno=%d)\n", std::strerror(errno), errno);
        return -1;
    }

    // 2. Enable SO_REUSEADDR on the listening socket.
    // When a TCP server shuts down or restarts, sockets recently closed enter the TIME_WAIT
    // state (typically 30-120 seconds) to ensure delayed packets in transit do not corrupt
    // subsequent connections. Without SO_REUSEADDR, re-binding to the same port immediately
    // results in EADDRINUSE ("Address already in use"). SO_REUSEADDR permits binding as long
    // as no active listening socket is already bound to the exact same local tuple.
    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::fprintf(stderr, "[ERROR] setsockopt(SO_REUSEADDR) failed: %s\n", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 3. Configure the address structure and bind to local address and port.
    // sockaddr_in defines IPv4 socket addressing.
    // sin_family must be AF_INET.
    // sin_port must be in Network Byte Order (Big Endian), converted via htons().
    // sin_addr is converted from text representation using inet_pton().
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);

    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        std::fprintf(stderr, "[ERROR] Invalid bind address '%s'\n", config_.host.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // bind() assigns the transport address (IP and port) to the socket file descriptor.
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[ERROR] bind() to %s:%u failed: %s (errno=%d)\n",
                     config_.host.c_str(), config_.port, std::strerror(errno), errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 4. Mark socket as passive listening socket to accept incoming connections.
    // listen() transitions the socket state from active (connecting) to passive (listening).
    // The second parameter (backlog) defines the maximum queue length for pending,
    // unaccepted connections (SYN_RCVD and ESTABLISHED queues). We use SOMAXCONN (Linux
    // system limit configured in /proc/sys/net/core/somaxconn, typically 4096 on modern Linux).
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        std::fprintf(stderr, "[ERROR] listen() failed: %s\n", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    std::printf("[INFO] tinykv listening on %s:%u (fd=%d, backlog=%d)\n",
                config_.host.c_str(), config_.port, listen_fd_, SOMAXCONN);
    return 0;
}

int Server::run() {
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "[ERROR] Server::run() called before successful init()\n");
        return -1;
    }

    running_ = true;

    // Blocking connection accept loop (M1 baseline)
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        // accept() blocks until an incoming connection arrives on the listening queue.
        // It returns a BRAND NEW file descriptor referring to the established connection
        // with this specific peer. The original listen_fd_ remains open and unchanged,
        // ready to accept more connections.
        int client_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // Interrupted by signal (e.g., SIGINT/SIGTERM for shutdown)
                continue;
            }
            if (!running_) {
                break;
            }
            std::fprintf(stderr, "[WARN] accept() failed: %s\n", std::strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        uint16_t client_port = ntohs(client_addr.sin_port);

        std::printf("[INFO] Accepted client connection from %s:%u (fd=%d)\n",
                    client_ip, client_port, client_fd);

        handle_client(client_fd);
    }

    return 0;
}

void Server::handle_client(int client_fd) {
    // Blocking echo loop:
    // Reads bytes from client_fd and immediately echoes them back.
    // TCP is a stream protocol without message boundaries:
    // - read() returns 0 on EOF (clean client disconnect / FIN).
    // - read() returns -1 on error.
    // - A single write() must loop until all read bytes are transmitted.
    constexpr size_t kBufferSize = 4096;
    char buffer[kBufferSize];

    while (running_) {
        ssize_t bytes_read = ::read(client_fd, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            // EOF: client sent FIN packet (closed write half of connection)
            std::printf("[INFO] Client on fd=%d disconnected cleanly (EOF)\n", client_fd);
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "[WARN] read() error on fd=%d: %s\n", client_fd, std::strerror(errno));
            break;
        }

        // Echo the exact bytes back to the client.
        // In TCP, write() may transmit fewer bytes than requested (short write),
        // so we loop until all bytes_read are confirmed written.
        size_t total_written = 0;
        const size_t to_write = static_cast<size_t>(bytes_read);
        bool write_error = false;

        while (total_written < to_write) {
            ssize_t written = ::write(client_fd, buffer + total_written, to_write - total_written);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::fprintf(stderr, "[WARN] write() error on fd=%d: %s\n", client_fd, std::strerror(errno));
                write_error = true;
                break;
            }
            total_written += static_cast<size_t>(written);
        }

        if (write_error) {
            break;
        }
    }

    // Close the client connection and release its file descriptor back to the OS table.
    ::close(client_fd);
}

void Server::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        std::printf("[INFO] tinykv listening socket closed.\n");
    }
}

} // namespace tinykv
