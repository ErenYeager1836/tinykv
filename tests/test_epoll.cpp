#include <arpa/inet.h>
#include <cerrno>
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

    for (int retry = 0; retry < 30; ++retry) {
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::usleep(50000); // 50ms
    }

    ::close(fd);
    return -1;
}

void test_concurrent_clients(const char* host, uint16_t port, int num_clients) {
    std::printf("[TEST] Connecting %d concurrent clients...\n", num_clients);

    std::vector<int> client_fds;
    client_fds.reserve(static_cast<size_t>(num_clients));

    for (int i = 0; i < num_clients; ++i) {
        int fd = connect_to_server(host, port);
        TEST_ASSERT(fd >= 0);
        client_fds.push_back(fd);
    }

    // Send ECHO command from each client
    for (int i = 0; i < num_clients; ++i) {
        char payload[32];
        std::snprintf(payload, sizeof(payload), "CLIENT_%d", i);
        size_t p_len = std::strlen(payload);

        char msg[128];
        std::snprintf(msg, sizeof(msg), "*2\r\n$4\r\nECHO\r\n$%zu\r\n%s\r\n", p_len, payload);
        size_t len = std::strlen(msg);

        ssize_t written = ::write(client_fds[static_cast<size_t>(i)], msg, len);
        TEST_ASSERT(written == static_cast<ssize_t>(len));
    }

    // Read back and verify exact bulk string echo on every client
    for (int i = 0; i < num_clients; ++i) {
        char payload[32];
        std::snprintf(payload, sizeof(payload), "CLIENT_%d", i);
        size_t p_len = std::strlen(payload);

        char expected[128];
        std::snprintf(expected, sizeof(expected), "$%zu\r\n%s\r\n", p_len, payload);
        size_t expected_len = std::strlen(expected);

        char recv_buf[128]{};
        size_t total_read = 0;
        while (total_read < expected_len) {
            ssize_t n = ::read(client_fds[static_cast<size_t>(i)],
                               recv_buf + total_read,
                               sizeof(recv_buf) - 1 - total_read);
            TEST_ASSERT(n > 0);
            total_read += static_cast<size_t>(n);
        }

        TEST_ASSERT(total_read == expected_len);
        TEST_ASSERT(std::memcmp(recv_buf, expected, expected_len) == 0);
    }

    for (int fd : client_fds) {
        ::close(fd);
    }

    std::printf("  [PASS] Concurrent clients multiplexing (%d clients verified)\n", num_clients);
}

void test_pipelined_messages(int fd) {
    // Send 10 pipelined PING commands back-to-back in a single write call
    std::string pipelined;
    std::string expected;
    for (int i = 0; i < 10; ++i) {
        pipelined += "*1\r\n$4\r\nPING\r\n";
        expected += "+PONG\r\n";
    }

    ssize_t written = ::write(fd, pipelined.data(), pipelined.size());
    TEST_ASSERT(written == static_cast<ssize_t>(pipelined.size()));

    std::vector<char> recv_buf(expected.size());
    size_t total_read = 0;
    while (total_read < expected.size()) {
        ssize_t n = ::read(fd, recv_buf.data() + total_read, expected.size() - total_read);
        TEST_ASSERT(n > 0);
        total_read += static_cast<size_t>(n);
    }

    TEST_ASSERT(total_read == expected.size());
    TEST_ASSERT(std::memcmp(recv_buf.data(), expected.data(), expected.size()) == 0);
    std::printf("  [PASS] Pipelined consecutive messages (10 commands in single write)\n");
}

void test_large_stream_buffering(int fd) {
    // 64 KB payload testing epoll non-blocking buffer accumulation and EPOLLOUT draining
    std::string payload(65536, 'x');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i % 95) + 32);
    }

    std::string req = "*2\r\n$4\r\nECHO\r\n$65536\r\n" + payload + "\r\n";
    std::string expected = "$65536\r\n" + payload + "\r\n";

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
    std::printf("  [PASS] Large stream buffering (64 KB RESP2 ECHO round-trip)\n");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <path-to-tinykv-binary>\n", argv[0]);
        return 1;
    }

    const char* server_bin = argv[1];
    constexpr uint16_t test_port = 16380;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", test_port);

    std::printf("[TEST] Spawning tinykv server on 127.0.0.1:%u...\n", test_port);

    pid_t pid = ::fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        ::execl(server_bin, server_bin, "--port", port_str, "--bind", "127.0.0.1", nullptr);
        std::perror("execl failed");
        std::exit(1);
    }

    // 1. Concurrent clients test (50 concurrent connections)
    test_concurrent_clients("127.0.0.1", test_port, 50);

    // 2. Connect client for pipelining and large stream tests
    int client_fd = connect_to_server("127.0.0.1", test_port);
    TEST_ASSERT(client_fd >= 0);

    test_pipelined_messages(client_fd);
    test_large_stream_buffering(client_fd);

    ::close(client_fd);

    // 3. Test clean server shutdown on SIGTERM with active connected clients
    std::printf("[TEST] Testing graceful shutdown with idle connected clients...\n");
    std::vector<int> idle_clients;
    for (int i = 0; i < 10; ++i) {
        int fd = connect_to_server("127.0.0.1", test_port);
        TEST_ASSERT(fd >= 0);
        idle_clients.push_back(fd);
    }

    std::printf("[TEST] Sending SIGTERM to pid=%d...\n", pid);
    ::kill(pid, SIGTERM);

    int status = 0;
    ::waitpid(pid, &status, 0);
    TEST_ASSERT(WIFEXITED(status));
    TEST_ASSERT(WEXITSTATUS(status) == 0);

    for (int fd : idle_clients) {
        char b[16];
        ssize_t n = ::read(fd, b, sizeof(b));
        TEST_ASSERT(n <= 0); // 0 (EOF) or -1 (connection reset)
        ::close(fd);
    }

    std::printf("[TEST] All M2 epoll event-loop tests PASSED.\n");
    return 0;
}
