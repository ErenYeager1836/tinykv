#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define TEST_ASSERT(cond)                                                                \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "[FAIL] Assertion failed: %s at %s:%d\n", #cond,       \
                         __FILE__, __LINE__);                                            \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {

int connect_to_server(const char* host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        ::close(fd);
        return -1;
    }

    for (int retry = 0; retry < 20; ++retry) {
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::usleep(50000); // 50ms
    }

    ::close(fd);
    return -1;
}

void test_small_echo(int fd) {
    const std::string msg = "*2\r\n$4\r\nECHO\r\n$4\r\nPING\r\n";
    const std::string expected = "$4\r\nPING\r\n";

    ssize_t written = ::write(fd, msg.data(), msg.size());
    TEST_ASSERT(written == static_cast<ssize_t>(msg.size()));

    char recv_buf[64]{};
    size_t total_read = 0;
    while (total_read < expected.size()) {
        ssize_t n = ::read(fd, recv_buf + total_read, expected.size() - total_read);
        TEST_ASSERT(n > 0);
        total_read += static_cast<size_t>(n);
    }

    TEST_ASSERT(expected == std::string(recv_buf, total_read));
    std::printf("  [PASS] test_small_echo (ECHO PING)\n");
}

void test_multi_block_echo(int fd) {
    // 8 KB payload testing stream fragmentation / buffering via RESP2 ECHO
    std::string payload(8192, 'x');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i % 94) + 33);
    }

    std::string req = "*2\r\n$4\r\nECHO\r\n$8192\r\n" + payload + "\r\n";
    std::string expected = "$8192\r\n" + payload + "\r\n";

    size_t total_sent = 0;
    while (total_sent < req.size()) {
        ssize_t sent = ::write(fd, req.data() + total_sent, req.size() - total_sent);
        TEST_ASSERT(sent > 0);
        total_sent += static_cast<size_t>(sent);
    }

    std::vector<char> recv_buf(expected.size());
    size_t total_read = 0;
    while (total_read < expected.size()) {
        ssize_t n = ::read(fd, recv_buf.data() + total_read, expected.size() - total_read);
        TEST_ASSERT(n > 0);
        total_read += static_cast<size_t>(n);
    }

    TEST_ASSERT(total_read == expected.size());
    TEST_ASSERT(expected == std::string(recv_buf.data(), recv_buf.size()));
    std::printf("  [PASS] test_multi_block_echo (8192 bytes via RESP2 ECHO)\n");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <path-to-tinykv-binary>\n", argv[0]);
        return 1;
    }

    const char* server_bin = argv[1];
    constexpr uint16_t test_port = 16379;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", test_port);

    std::printf("[TEST] Launching %s on 127.0.0.1:%u...\n", server_bin, test_port);

    pid_t pid = ::fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        ::execl(server_bin, server_bin, "--port", port_str, "--bind", "127.0.0.1", nullptr);
        std::perror("execl failed");
        std::exit(1);
    }

    int client_fd = connect_to_server("127.0.0.1", test_port);
    if (client_fd < 0) {
        std::fprintf(stderr, "[FAIL] Could not connect to tinykv\n");
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        return 1;
    }

    test_small_echo(client_fd);
    test_multi_block_echo(client_fd);

    ::close(client_fd);

    std::printf("[TEST] Shutting down tinykv server (pid=%d)...\n", pid);
    ::kill(pid, SIGTERM);

    int status = 0;
    ::waitpid(pid, &status, 0);
    TEST_ASSERT(WIFEXITED(status) || WIFSIGNALED(status));

    std::printf("[TEST] All M1 echo tests PASSED.\n");
    return 0;
}
