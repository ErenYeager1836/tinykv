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

std::string send_and_recv(int fd, const std::string& request, size_t expected_bytes) {
    ssize_t w = ::write(fd, request.data(), request.size());
    TEST_ASSERT(w == static_cast<ssize_t>(request.size()));

    std::string response;
    response.resize(expected_bytes);

    size_t total_read = 0;
    while (total_read < expected_bytes) {
        ssize_t n = ::read(fd, &response[total_read], expected_bytes - total_read);
        TEST_ASSERT(n > 0);
        total_read += static_cast<size_t>(n);
    }

    return response;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <path-to-tinykv-binary>\n", argv[0]);
        return 1;
    }

    const char* server_bin = argv[1];
    constexpr uint16_t test_port = 16381;
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
    TEST_ASSERT(client_fd >= 0);

    // 1. PING without args -> +PONG\r\n
    std::string res = send_and_recv(client_fd, "*1\r\n$4\r\nPING\r\n", 7);
    TEST_ASSERT(res == "+PONG\r\n");
    std::printf("  [PASS] PING -> +PONG\r\n");

    // 2. PING with message -> bulk string reply
    std::string ping_msg = "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n";
    std::string ping_exp = "$5\r\nhello\r\n";
    res = send_and_recv(client_fd, ping_msg, ping_exp.size());
    TEST_ASSERT(res == ping_exp);
    std::printf("  [PASS] PING hello -> $5\\r\\nhello\\r\\n\n");

    // 3. ECHO message
    std::string echo_cmd = "*2\r\n$4\r\nECHO\r\n$12\r\nhello world!\r\n";
    std::string echo_exp = "$12\r\nhello world!\r\n";
    res = send_and_recv(client_fd, echo_cmd, echo_exp.size());
    TEST_ASSERT(res == echo_exp);
    std::printf("  [PASS] ECHO \"hello world!\"\n");

    // 4. SET key value
    std::string set_cmd = "*3\r\n$3\r\nSET\r\n$6\r\nuser:1\r\n$5\r\nAlice\r\n";
    res = send_and_recv(client_fd, set_cmd, 5);
    TEST_ASSERT(res == "+OK\r\n");
    std::printf("  [PASS] SET user:1 Alice -> +OK\r\n");

    // 5. GET key
    std::string get_cmd = "*2\r\n$3\r\nGET\r\n$6\r\nuser:1\r\n";
    std::string get_exp = "$5\r\nAlice\r\n";
    res = send_and_recv(client_fd, get_cmd, get_exp.size());
    TEST_ASSERT(res == get_exp);
    std::printf("  [PASS] GET user:1 -> Alice\n");

    // 6. GET non-existent key -> $-1\r\n
    std::string get_missing = "*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n";
    res = send_and_recv(client_fd, get_missing, 5);
    TEST_ASSERT(res == "$-1\r\n");
    std::printf("  [PASS] GET missing -> $-1\\r\\n (nil)\n");

    // 7. Case-insensitivity: lowercase 'set' and mixed-case 'gEt'
    std::string set_lower = "*3\r\n$3\r\nset\r\n$4\r\ncity\r\n$5\r\nTokyo\r\n";
    res = send_and_recv(client_fd, set_lower, 5);
    TEST_ASSERT(res == "+OK\r\n");

    std::string get_mixed = "*2\r\n$3\r\ngEt\r\n$4\r\ncity\r\n";
    std::string get_mixed_exp = "$5\r\nTokyo\r\n";
    res = send_and_recv(client_fd, get_mixed, get_mixed_exp.size());
    TEST_ASSERT(res == get_mixed_exp);
    std::printf("  [PASS] Case-insensitivity ('set' and 'gEt')\n");

    // 8. Tooling startup probing compatibility (COMMAND -> *0\r\n)
    std::string cmd_probe = "*1\r\n$7\r\nCOMMAND\r\n";
    res = send_and_recv(client_fd, cmd_probe, 4);
    TEST_ASSERT(res == "*0\r\n");
    std::printf("  [PASS] COMMAND -> *0\\r\\n (empty array)\n");

    // 9. Inline command: PING\r\n
    res = send_and_recv(client_fd, "PING\r\n", 7);
    TEST_ASSERT(res == "+PONG\r\n");
    std::printf("  [PASS] Inline PING -> +PONG\r\n");

    // 10. QUIT -> +OK\r\n and socket disconnect
    res = send_and_recv(client_fd, "*1\r\n$4\r\nQUIT\r\n", 5);
    TEST_ASSERT(res == "+OK\r\n");

    char buf[16];
    ssize_t n = ::read(client_fd, buf, sizeof(buf));
    TEST_ASSERT(n == 0); // Clean EOF from server
    ::close(client_fd);
    std::printf("  [PASS] QUIT -> +OK and clean server disconnect\n");

    // Shutdown server
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);

    std::printf("[TEST] All M3 end-to-end command tests PASSED.\n");
    return 0;
}
