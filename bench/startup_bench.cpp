#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace {

using Clock = std::chrono::steady_clock;

double measure_one_startup(const char* bin_path, const char* server_type, uint16_t port) {
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    auto start_time = Clock::now();

    pid_t pid = ::fork();
    if (pid == 0) {
        // Redirect stdout and stderr to /dev/null during measurement
        FILE* devnull = std::fopen("/dev/null", "w");
        if (devnull) {
            ::dup2(fileno(devnull), STDOUT_FILENO);
            ::dup2(fileno(devnull), STDERR_FILENO);
            std::fclose(devnull);
        }

        if (std::strcmp(server_type, "tinykv") == 0) {
            ::execl(bin_path, bin_path, "--port", port_str, "--bind", "127.0.0.1", nullptr);
        } else {
            // valkey / redis
            ::execl(bin_path, bin_path, "--port", port_str, "--bind", "127.0.0.1",
                    "--save", "", "--appendonly", "no", "--protected-mode", "no", nullptr);
        }
        std::exit(1);
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int client_fd = -1;
    constexpr int kMaxAttempts = 5000; // up to ~500ms
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd >= 0) {
            if (::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                // Connected! Send PING\r\n
                const char req[] = "PING\r\n";
                if (::write(client_fd, req, sizeof(req) - 1) == sizeof(req) - 1) {
                    char buf[32];
                    ssize_t n = ::read(client_fd, buf, sizeof(buf));
                    if (n > 0 && std::memcmp(buf, "+PONG\r\n", 7) == 0) {
                        // Received first PONG reply!
                        auto end_time = Clock::now();
                        ::close(client_fd);
                        ::kill(pid, SIGTERM);
                        int status = 0;
                        ::waitpid(pid, &status, 0);
                        return std::chrono::duration<double, std::milli>(end_time - start_time).count();
                    }
                }
                ::close(client_fd);
            } else {
                ::close(client_fd);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Timeout
    if (client_fd >= 0) ::close(client_fd);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return -1.0;
}

double compute_median(std::vector<double>& v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    size_t mid = v.size() / 2;
    if (v.size() % 2 == 1) return v[mid];
    return (v[mid - 1] + v[mid]) / 2.0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <binary-path> <server-type: tinykv|valkey> <port> [runs=10]\n", argv[0]);
        return 1;
    }

    const char* bin_path = argv[1];
    const char* server_type = argv[2];
    uint16_t port = static_cast<uint16_t>(std::atoi(argv[3]));
    int runs = (argc >= 5) ? std::atoi(argv[4]) : 10;

    std::vector<double> timings;
    timings.reserve(static_cast<size_t>(runs));

    for (int r = 0; r < runs; ++r) {
        double ms = measure_one_startup(bin_path, server_type, port);
        if (ms > 0.0) {
            timings.push_back(ms);
        }
        // Small pause between spawns to avoid port collision in TIME_WAIT
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (timings.empty()) {
        std::fprintf(stderr, "[ERROR] Failed to measure startup time for %s\n", bin_path);
        return 1;
    }

    double median_ms = compute_median(timings);
    std::printf("%s: median=%.2f ms (min=%.2f ms, max=%.2f ms, runs=%zu)\n",
                server_type, median_ms, timings.front(), timings.back(), timings.size());
    return 0;
}
