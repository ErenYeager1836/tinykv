#pragma once

#include "commands.h"
#include "resp.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tinykv {

// Hard limit for buffer size per client connection (1 MB) to prevent denial-of-service / OOM attacks
constexpr size_t kMaxClientBufferSize = 1024 * 1024;
constexpr int kDefaultEpollEvents = 64;

struct ServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    int max_clients = 1024;
};

// Represents state for an active non-blocking client connection
struct Connection {
    int fd{-1};
    std::vector<uint8_t> read_buf;
    std::vector<uint8_t> write_buf;
    size_t write_offset{0};
    uint32_t epoll_events{0};
    bool close_after_write{false};

    explicit Connection(int sock_fd) : fd(sock_fd) {
        read_buf.reserve(4096);
        write_buf.reserve(4096);
    }
};

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    // Disable copy and move semantics (owns OS file descriptors)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Initialize listening socket and epoll instance
    // Returns 0 on success, negative error code on failure.
    int init();

    // Start single-threaded non-blocking epoll event loop
    int run();

    // Request graceful shutdown (signal-safe)
    void stop();

    // Query active state
    int listen_fd() const { return listen_fd_; }
    int epoll_fd() const { return epoll_fd_; }
    int active_clients() const { return active_clients_; }
    bool is_connected(int fd) const {
        if (fd < 0 || static_cast<size_t>(fd) >= conns_.size()) {
            return false;
        }
        return conns_[static_cast<size_t>(fd)] != nullptr;
    }

private:
    ServerConfig config_;
    int listen_fd_{-1};
    int epoll_fd_{-1};
    std::atomic<bool> running_{false};
    int active_clients_{0};

    // Fast O(1) connection table indexed directly by client file descriptor
    std::vector<std::unique_ptr<Connection>> conns_;

    // Command dispatch engine
    CommandDispatcher dispatcher_;

    // Socket and epoll helpers
    int set_nonblocking(int fd);
    int epoll_add(int fd, uint32_t events);
    int epoll_mod(int fd, uint32_t events);
    int epoll_del(int fd);

    // Event handlers
    void handle_accept();
    void handle_read(int fd);
    void handle_write(int fd);
    void close_connection(int fd);
    void cleanup_all_connections();
};

} // namespace tinykv
