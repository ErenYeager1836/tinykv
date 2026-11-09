#include "server.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

tinykv::Server* g_server_ptr = nullptr;

void signal_handler(int signum) {
    if (g_server_ptr) {
        std::printf("\n[INFO] Caught signal %d (%s), initiating clean shutdown...\n",
                    signum, ::strsignal(signum));
        g_server_ptr->stop();
    }
}

void print_usage(const char* prog_name) {
    std::printf("Usage: %s [OPTIONS]\n\n", prog_name);
    std::printf("A minimal, high-performance Redis-compatible key-value server.\n\n");
    std::printf("Options:\n");
    std::printf("  -p, --port <port>         TCP port to listen on (default: 6379)\n");
    std::printf("  -b, --bind <ip>           IP address to bind to (default: 127.0.0.1)\n");
    std::printf("  -m, --max-clients <num>   Maximum concurrent clients (default: 1024)\n");
    std::printf("  -h, --help                Show this help message and exit\n");
}

} // namespace

int main(int argc, char* argv[]) {
    tinykv::ServerConfig config{};

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            long p = std::strtol(argv[++i], nullptr, 10);
            if (p <= 0 || p > 65535) {
                std::fprintf(stderr, "[ERROR] Invalid port '%s': must be 1-65535\n", argv[i]);
                return 1;
            }
            config.port = static_cast<uint16_t>(p);
        } else if ((std::strcmp(argv[i], "-b") == 0 || std::strcmp(argv[i], "--bind") == 0) && i + 1 < argc) {
            config.host = argv[++i];
        } else if ((std::strcmp(argv[i], "-m") == 0 || std::strcmp(argv[i], "--max-clients") == 0) && i + 1 < argc) {
            long m = std::strtol(argv[++i], nullptr, 10);
            if (m <= 0 || m > 100000) {
                std::fprintf(stderr, "[ERROR] Invalid max-clients '%s'\n", argv[i]);
                return 1;
            }
            config.max_clients = static_cast<int>(m);
        } else {
            std::fprintf(stderr, "[ERROR] Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    tinykv::Server server(config);
    g_server_ptr = &server;

    // Set up signal handlers for graceful shutdown
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (::sigaction(SIGINT, &sa, nullptr) < 0 || ::sigaction(SIGTERM, &sa, nullptr) < 0) {
        std::fprintf(stderr, "[WARN] Failed to configure signal handlers\n");
    }

    // Ignore SIGPIPE so writing to a disconnected peer does not abort the process
    ::signal(SIGPIPE, SIG_IGN);

    if (server.init() != 0) {
        return 1;
    }

    std::printf("[INFO] tinykv M3 RESP2 server ready.\n");
    int rc = server.run();
    g_server_ptr = nullptr;

    return rc;
}
