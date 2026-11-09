#include "server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tinykv {

Server::Server(ServerConfig config)
    : config_(std::move(config)) {}

Server::~Server() {
    stop();
}

int Server::set_nonblocking(int fd) {
    // Retrieve current socket file status flags
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::fprintf(stderr, "[ERROR] fcntl(F_GETFL, fd=%d) failed: %s\n", fd, std::strerror(errno));
        return -1;
    }
    // Set O_NONBLOCK: subsequent read(), write(), or accept() operations will
    // return immediately with EAGAIN/EWOULDBLOCK instead of blocking the thread.
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "[ERROR] fcntl(F_SETFL, O_NONBLOCK, fd=%d) failed: %s\n", fd, std::strerror(errno));
        return -1;
    }
    return 0;
}

int Server::epoll_add(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::fprintf(stderr, "[ERROR] epoll_ctl(ADD, fd=%d) failed: %s\n", fd, std::strerror(errno));
        return -1;
    }
    return 0;
}

int Server::epoll_mod(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        std::fprintf(stderr, "[ERROR] epoll_ctl(MOD, fd=%d) failed: %s\n", fd, std::strerror(errno));
        return -1;
    }
    return 0;
}

int Server::epoll_del(int fd) {
    struct epoll_event ev{};
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) < 0) {
        return -1;
    }
    return 0;
}

int Server::init() {
    // 1. Create IPv4 TCP socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "[ERROR] socket() failed: %s (errno=%d)\n", std::strerror(errno), errno);
        return -1;
    }

    // 2. SO_REUSEADDR allows immediate re-binding during TIME_WAIT state
    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::fprintf(stderr, "[ERROR] setsockopt(SO_REUSEADDR) failed: %s\n", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 3. Make listening socket non-blocking.
    // If a client sends SYN then aborts (RST) before accept() runs, a blocking
    // accept() would freeze the entire event loop. Non-blocking mode ensures
    // accept() returns immediately with EAGAIN if no connection remains queued.
    if (set_nonblocking(listen_fd_) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 4. Bind address and port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);

    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        std::fprintf(stderr, "[ERROR] Invalid bind address '%s'\n", config_.host.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[ERROR] bind() to %s:%u failed: %s (errno=%d)\n",
                     config_.host.c_str(), config_.port, std::strerror(errno), errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 5. Mark socket passive with SOMAXCONN backlog queue
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        std::fprintf(stderr, "[ERROR] listen() failed: %s\n", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 6. Create epoll instance with EPOLL_CLOEXEC
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::fprintf(stderr, "[ERROR] epoll_create1 failed: %s\n", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    // 7. Register listening socket for EPOLLIN (incoming connection readiness)
    if (epoll_add(listen_fd_, EPOLLIN) < 0) {
        ::close(epoll_fd_);
        ::close(listen_fd_);
        epoll_fd_ = -1;
        listen_fd_ = -1;
        return -1;
    }

    // Pre-allocate connection table to comfortably index by file descriptor
    conns_.resize(static_cast<size_t>(config_.max_clients + 128));

    std::printf("[INFO] tinykv listening on %s:%u (fd=%d, epoll_fd=%d, max_clients=%d)\n",
                config_.host.c_str(), config_.port, listen_fd_, epoll_fd_, config_.max_clients);
    return 0;
}

int Server::run() {
    if (listen_fd_ < 0 || epoll_fd_ < 0) {
        std::fprintf(stderr, "[ERROR] Server::run() called before successful init()\n");
        return -1;
    }

    running_ = true;
    struct epoll_event events[kDefaultEpollEvents];

    while (running_) {
        // Wait indefinitely until I/O events occur or a signal interrupts the wait
        int nfds = ::epoll_wait(epoll_fd_, events, kDefaultEpollEvents, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                // Interrupted by signal (SIGINT, SIGTERM)
                if (!running_) {
                    break;
                }
                continue;
            }
            std::fprintf(stderr, "[ERROR] epoll_wait error: %s (errno=%d)\n", std::strerror(errno), errno);
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd_) {
                // Ready for new client connections
                handle_accept();
            } else {
                // Client socket event
                if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    close_connection(fd);
                    continue;
                }

                if (ev & EPOLLIN) {
                    handle_read(fd);
                }

                // If connection remains open and has pending write events
                if ((ev & EPOLLOUT) && is_connected(fd)) {
                    handle_write(fd);
                }
            }
        }
    }

    cleanup_all_connections();
    return 0;
}

void Server::handle_accept() {
    // Level-triggered: loop accept4() until EAGAIN to drain all queued connections
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        // accept4 creates the socket atomically with non-blocking and close-on-exec flags
        int client_fd = ::accept4(listen_fd_,
                                  reinterpret_cast<struct sockaddr*>(&client_addr),
                                  &client_len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // All pending connections have been accepted
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                std::fprintf(stderr, "[WARN] File descriptor limit reached during accept4()\n");
                break;
            }
            std::fprintf(stderr, "[WARN] accept4() failed: %s\n", std::strerror(errno));
            break;
        }

        // Hard limit on maximum concurrent clients
        if (active_clients_ >= config_.max_clients) {
            std::fprintf(stderr, "[WARN] Max clients limit (%d) reached. Dropping fd=%d\n",
                         config_.max_clients, client_fd);
            ::close(client_fd);
            continue;
        }

        // Ensure connection table has room for this file descriptor
        if (static_cast<size_t>(client_fd) >= conns_.size()) {
            conns_.resize(static_cast<size_t>(client_fd) + 128);
        }

        // Initialize connection state
        auto conn = std::make_unique<Connection>(client_fd);
        conn->epoll_events = EPOLLIN | EPOLLRDHUP;

        // Register in epoll with EPOLLIN and EPOLLRDHUP (fast disconnect detection)
        if (epoll_add(client_fd, conn->epoll_events) < 0) {
            ::close(client_fd);
            continue;
        }

        conns_[static_cast<size_t>(client_fd)] = std::move(conn);
        ++active_clients_;

        char client_ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        uint16_t client_port = ntohs(client_addr.sin_port);

        std::printf("[INFO] Accepted non-blocking client from %s:%u (fd=%d, active=%d)\n",
                    client_ip, client_port, client_fd, active_clients_);
    }
}

void Server::handle_read(int fd) {
    if (!is_connected(fd)) {
        return;
    }
    Connection& conn = *conns_[static_cast<size_t>(fd)];

    uint8_t buffer[4096];
    while (true) {
        ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            // Enforce maximum buffer limit per client to prevent OOM DOS
            if (conn.read_buf.size() + static_cast<size_t>(bytes_read) > kMaxClientBufferSize) {
                std::fprintf(stderr, "[WARN] Client fd=%d exceeded buffer limit (%zu bytes). Terminating.\n",
                             fd, kMaxClientBufferSize);
                close_connection(fd);
                return;
            }

            conn.read_buf.insert(conn.read_buf.end(), buffer, buffer + bytes_read);
        } else if (bytes_read == 0) {
            // EOF: client performed clean shutdown
            close_connection(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket receive buffer drained
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            // Error reading socket (e.g. ECONNRESET)
            close_connection(fd);
            return;
        }
    }

    // In M2: Echo mode (transfer read buffer directly to write buffer)
    if (!conn.read_buf.empty()) {
        conn.write_buf.insert(conn.write_buf.end(), conn.read_buf.begin(), conn.read_buf.end());
        conn.read_buf.clear();

        // Attempt non-blocking write immediately
        handle_write(fd);
    }
}

void Server::handle_write(int fd) {
    if (!is_connected(fd)) {
        return;
    }
    Connection& conn = *conns_[static_cast<size_t>(fd)];

    while (conn.write_offset < conn.write_buf.size()) {
        const size_t to_write = conn.write_buf.size() - conn.write_offset;
        ssize_t written = ::write(fd, conn.write_buf.data() + conn.write_offset, to_write);

        if (written > 0) {
            conn.write_offset += static_cast<size_t>(written);
        } else if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Kernel send buffer is full. Register EPOLLOUT so epoll notifies
                // us when the socket becomes writable again.
                if (!(conn.epoll_events & EPOLLOUT)) {
                    conn.epoll_events |= EPOLLOUT;
                    epoll_mod(fd, conn.epoll_events);
                }
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            // Connection broken (e.g. EPIPE)
            close_connection(fd);
            return;
        }
    }

    // Full write buffer has been flushed to the socket
    conn.write_buf.clear();
    conn.write_offset = 0;

    // CRITICAL for level-triggered epoll:
    // If EPOLLOUT is enabled, epoll_wait will trigger continuously (sockets are
    // almost always writable). Once all data is written, EPOLLOUT MUST be cleared.
    if (conn.epoll_events & EPOLLOUT) {
        conn.epoll_events &= ~static_cast<uint32_t>(EPOLLOUT);
        epoll_mod(fd, conn.epoll_events);
    }
}

void Server::close_connection(int fd) {
    if (!is_connected(fd)) {
        return;
    }

    // Unregister from epoll
    epoll_del(fd);

    // Close socket
    ::close(fd);

    // Free connection state
    conns_[static_cast<size_t>(fd)].reset();
    --active_clients_;

    std::printf("[INFO] Closed connection fd=%d (active=%d)\n", fd, active_clients_);
}

void Server::cleanup_all_connections() {
    for (size_t i = 0; i < conns_.size(); ++i) {
        if (conns_[i]) {
            int fd = conns_[i]->fd;
            epoll_del(fd);
            ::close(fd);
            conns_[i].reset();
        }
    }
    active_clients_ = 0;
}

void Server::stop() {
    running_ = false;

    cleanup_all_connections();

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        std::printf("[INFO] tinykv listening socket closed.\n");
    }
}

} // namespace tinykv
