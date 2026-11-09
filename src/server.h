#pragma once

#include <cstdint>
#include <string>

namespace tinykv {

struct ServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    int max_clients = 1024;
};

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    // Disable copy and move semantics (owns OS file descriptor)
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Initialize listening socket: socket() -> setsockopt() -> bind() -> listen()
    // Returns 0 on success, negative error code on failure.
    int init();

    // Start blocking connection accept loop
    // In M1, this serves connections sequentially to establish baseline socket mechanics.
    // In M2, this evolves into an epoll-driven non-blocking event loop.
    int run();

    // Request graceful shutdown
    void stop();

    // Query active listening file descriptor
    int listen_fd() const { return listen_fd_; }

private:
    ServerConfig config_;
    int listen_fd_{-1};
    volatile bool running_{false};

    // Handle an individual client connection in blocking echo mode (M1 baseline)
    void handle_client(int client_fd);
};

} // namespace tinykv
