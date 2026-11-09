#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>

#define TEST_ASSERT(cond)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond,        \
                         __FILE__, __LINE__);                                      \
            std::abort();                                                          \
        }                                                                          \
    } while (0)

namespace {

int connect_to_server(const char* host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string send_and_recv(int fd, std::string_view req, size_t expected_bytes) {
    size_t total_sent = 0;
    while (total_sent < req.size()) {
        ssize_t n = ::write(fd, req.data() + total_sent, req.size() - total_sent);
        TEST_ASSERT(n > 0);
        total_sent += static_cast<size_t>(n);
    }

    std::string res;
    res.resize(expected_bytes);
    size_t total_read = 0;
    while (total_read < expected_bytes) {
        ssize_t n = ::read(fd, res.data() + total_read, expected_bytes - total_read);
        TEST_ASSERT(n > 0);
        total_read += static_cast<size_t>(n);
    }
    return res;
}

std::string send_cmd(int fd, std::string_view req) {
    size_t total_sent = 0;
    while (total_sent < req.size()) {
        ssize_t n = ::write(fd, req.data() + total_sent, req.size() - total_sent);
        TEST_ASSERT(n > 0);
        total_sent += static_cast<size_t>(n);
    }

    std::string res;
    char buf[512];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        TEST_ASSERT(n > 0);
        res.append(buf, static_cast<size_t>(n));
        // Check if we reached \r\n at end of reply
        if (res.size() >= 2 && res[res.size() - 2] == '\r' && res[res.size() - 1] == '\n') {
            // If it's a bulk string, make sure we have all data
            if (res[0] == '$') {
                if (res.rfind("$-1\r\n", 0) == 0) {
                    break; // null bulk string
                }
                size_t first_crlf = res.find("\r\n");
                if (first_crlf != std::string::npos) {
                    int len = std::stoi(res.substr(1, first_crlf - 1));
                    if (res.size() >= first_crlf + 2 + static_cast<size_t>(len) + 2) {
                        break;
                    }
                }
            } else {
                break;
            }
        }
    }
    return res;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <path-to-tinykv-binary>\n", argv[0]);
        return 1;
    }

    const char* binary_path = argv[1];
    constexpr uint16_t kPort = 16382;
    constexpr const char* kHost = "127.0.0.1";

    std::printf("[TEST] Launching %s on %s:%u...\n", binary_path, kHost, kPort);
    pid_t pid = ::fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%u", kPort);
        ::execl(binary_path, binary_path, "--port", port_str, "--bind", kHost, nullptr);
        std::perror("execl failed");
        std::exit(1);
    }

    // Wait for server socket readiness
    int client_fd = -1;
    for (int i = 0; i < 50; ++i) {
        client_fd = connect_to_server(kHost, kPort);
        if (client_fd >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    TEST_ASSERT(client_fd >= 0);

    //=========================================================================
    // 1. Connection & Core Ping/Echo Tests
    //=========================================================================
    std::string res = send_and_recv(client_fd, "*1\r\n$4\r\nPING\r\n", 7);
    TEST_ASSERT(res == "+PONG\r\n");
    std::printf("  [PASS] PING -> +PONG\n");

    std::string ping_msg = "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n";
    res = send_and_recv(client_fd, ping_msg, 11);
    TEST_ASSERT(res == "$5\r\nhello\r\n");
    std::printf("  [PASS] PING hello -> $5\\r\\nhello\\r\\n\n");

    std::string echo_cmd = "*2\r\n$4\r\nECHO\r\n$12\r\nhello world!\r\n";
    res = send_and_recv(client_fd, echo_cmd, 19);
    TEST_ASSERT(res == "$12\r\nhello world!\r\n");
    std::printf("  [PASS] ECHO \"hello world!\"\n");

    //=========================================================================
    // 2. SET, GET, and DBSIZE
    //=========================================================================
    res = send_cmd(client_fd, "*1\r\n$6\r\nDBSIZE\r\n");
    TEST_ASSERT(res == ":0\r\n");
    std::printf("  [PASS] DBSIZE initially 0\n");

    res = send_cmd(client_fd, "*3\r\n$3\r\nSET\r\n$6\r\nuser:1\r\n$5\r\nAlice\r\n");
    TEST_ASSERT(res == "+OK\r\n");
    res = send_cmd(client_fd, "*3\r\n$3\r\nSET\r\n$6\r\nuser:2\r\n$3\r\nBob\r\n");
    TEST_ASSERT(res == "+OK\r\n");

    res = send_cmd(client_fd, "*1\r\n$6\r\nDBSIZE\r\n");
    TEST_ASSERT(res == ":2\r\n");
    std::printf("  [PASS] SET user:1 & user:2, DBSIZE -> 2\n");

    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$6\r\nuser:1\r\n");
    TEST_ASSERT(res == "$5\r\nAlice\r\n");
    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$6\r\nuser:2\r\n");
    TEST_ASSERT(res == "$3\r\nBob\r\n");
    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n");
    TEST_ASSERT(res == "$-1\r\n");
    std::printf("  [PASS] GET existing and missing keys\n");

    //=========================================================================
    // 3. EXISTS & DEL
    //=========================================================================
    // Single and multi-key EXISTS
    res = send_cmd(client_fd, "*2\r\n$6\r\nEXISTS\r\n$6\r\nuser:1\r\n");
    TEST_ASSERT(res == ":1\r\n");
    res = send_cmd(client_fd, "*3\r\n$6\r\nEXISTS\r\n$6\r\nuser:1\r\n$6\r\nuser:2\r\n");
    TEST_ASSERT(res == ":2\r\n");
    res = send_cmd(client_fd, "*3\r\n$6\r\nEXISTS\r\n$6\r\nuser:1\r\n$7\r\nmissing\r\n");
    TEST_ASSERT(res == ":1\r\n");
    std::printf("  [PASS] EXISTS single and multiple keys\n");

    // DEL multiple keys
    res = send_cmd(client_fd, "*3\r\n$3\r\nDEL\r\n$6\r\nuser:1\r\n$7\r\nmissing\r\n");
    TEST_ASSERT(res == ":1\r\n"); // 1 deleted, 1 missing
    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$6\r\nuser:1\r\n");
    TEST_ASSERT(res == "$-1\r\n");
    res = send_cmd(client_fd, "*1\r\n$6\r\nDBSIZE\r\n");
    TEST_ASSERT(res == ":1\r\n");
    std::printf("  [PASS] DEL keys and DBSIZE decrement\n");

    //=========================================================================
    // 4. INCR & DECR
    //=========================================================================
    // INCR non-existent key starts from 0 -> 1
    res = send_cmd(client_fd, "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n");
    TEST_ASSERT(res == ":1\r\n");
    res = send_cmd(client_fd, "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n");
    TEST_ASSERT(res == ":2\r\n");
    res = send_cmd(client_fd, "*2\r\n$4\r\nDECR\r\n$7\r\ncounter\r\n");
    TEST_ASSERT(res == ":1\r\n");
    res = send_cmd(client_fd, "*2\r\n$4\r\nDECR\r\n$7\r\ncounter\r\n");
    TEST_ASSERT(res == ":0\r\n");
    res = send_cmd(client_fd, "*2\r\n$4\r\nDECR\r\n$7\r\ncounter\r\n");
    TEST_ASSERT(res == ":-1\r\n");
    std::printf("  [PASS] INCR and DECR counter (including negative transitions)\n");

    // INCR on non-integer string returns error
    res = send_cmd(client_fd, "*2\r\n$4\r\nINCR\r\n$6\r\nuser:2\r\n"); // user:2 is "Bob"
    TEST_ASSERT(res.find("-ERR") != std::string::npos);
    std::printf("  [PASS] INCR on non-integer value returns -ERR\n");

    //=========================================================================
    // 5. Expiration & TTL (SET EX/PX, EXPIRE, TTL, Lazy Expiry)
    //=========================================================================
    // SET with EX (1 second)
    res = send_cmd(client_fd, "*5\r\n$3\r\nSET\r\n$10\r\nexpire_sec\r\n$5\r\nquick\r\n$2\r\nEX\r\n$1\r\n1\r\n");
    TEST_ASSERT(res == "+OK\r\n");

    res = send_cmd(client_fd, "*2\r\n$3\r\nTTL\r\n$10\r\nexpire_sec\r\n");
    TEST_ASSERT(res == ":1\r\n");

    // Immediate GET succeeds
    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$10\r\nexpire_sec\r\n");
    TEST_ASSERT(res == "$5\r\nquick\r\n");

    // Wait 1.1s for expiration
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Lazy expiry on GET
    res = send_cmd(client_fd, "*2\r\n$3\r\nGET\r\n$10\r\nexpire_sec\r\n");
    TEST_ASSERT(res == "$-1\r\n");
    res = send_cmd(client_fd, "*2\r\n$3\r\nTTL\r\n$10\r\nexpire_sec\r\n");
    TEST_ASSERT(res == ":-2\r\n");
    std::printf("  [PASS] SET EX and lazy expiration eviction\n");

    // EXPIRE on existing key
    res = send_cmd(client_fd, "*3\r\n$3\r\nSET\r\n$9\r\nttl_check\r\n$5\r\nhello\r\n");
    TEST_ASSERT(res == "+OK\r\n");
    res = send_cmd(client_fd, "*2\r\n$3\r\nTTL\r\n$9\r\nttl_check\r\n");
    TEST_ASSERT(res == ":-1\r\n"); // No expiry yet

    res = send_cmd(client_fd, "*3\r\n$6\r\nEXPIRE\r\n$9\r\nttl_check\r\n$2\r\n10\r\n");
    TEST_ASSERT(res == ":1\r\n");
    res = send_cmd(client_fd, "*2\r\n$3\r\nTTL\r\n$9\r\nttl_check\r\n");
    TEST_ASSERT(res == ":10\r\n");
    std::printf("  [PASS] EXPIRE command and TTL retrieval\n");

    //=========================================================================
    // 6. Active Expiration (eviction without client GET)
    //=========================================================================
    // Insert 50 short-lived keys with PX 100ms
    for (int i = 0; i < 50; ++i) {
        std::string k = "act_exp_" + std::to_string(i);
        std::string set_px = "*5\r\n$3\r\nSET\r\n$" + std::to_string(k.size()) + "\r\n" + k +
                             "\r\n$3\r\nval\r\n$2\r\nPX\r\n$3\r\n100\r\n";
        res = send_cmd(client_fd, set_px);
        TEST_ASSERT(res == "+OK\r\n");
    }

    // Wait 250ms for background active expiry cycles in epoll event loop
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // Verify through active expiry step that keys were evicted
    for (int i = 0; i < 50; ++i) {
        std::string k = "act_exp_" + std::to_string(i);
        std::string exists_cmd = "*2\r\n$6\r\nEXISTS\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
        res = send_cmd(client_fd, exists_cmd);
        TEST_ASSERT(res == ":0\r\n");
    }
    std::printf("  [PASS] Active background expiry eviction (50 keys purged)\n");

    //=========================================================================
    // 7. Pipelining (50 pipelined commands in a single TCP write)
    //=========================================================================
    std::string pipeline_req;
    for (int i = 0; i < 50; ++i) {
        std::string k = "pipe_" + std::to_string(i);
        std::string v = "val_" + std::to_string(i);
        pipeline_req += "*3\r\n$3\r\nSET\r\n$" + std::to_string(k.size()) + "\r\n" + k +
                        "\r\n$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
    }

    // Send all 50 SET commands at once
    size_t expected_set_replies = 50 * 5; // 50 * "+OK\r\n"
    res = send_and_recv(client_fd, pipeline_req, expected_set_replies);
    for (size_t i = 0; i < 50; ++i) {
        TEST_ASSERT(res.substr(i * 5, 5) == "+OK\r\n");
    }

    // Pipeline 50 GET commands at once
    pipeline_req.clear();
    std::string expected_get_replies;
    for (int i = 0; i < 50; ++i) {
        std::string k = "pipe_" + std::to_string(i);
        std::string v = "val_" + std::to_string(i);
        pipeline_req += "*2\r\n$3\r\nGET\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
        expected_get_replies += "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
    }

    res = send_and_recv(client_fd, pipeline_req, expected_get_replies.size());
    TEST_ASSERT(res == expected_get_replies);
    std::printf("  [PASS] Pipelining (50 SETs + 50 GETs in contiguous packet bursts)\n");

    //=========================================================================
    // 8. QUIT & Graceful Disconnect
    //=========================================================================
    res = send_and_recv(client_fd, "*1\r\n$4\r\nQUIT\r\n", 5);
    TEST_ASSERT(res == "+OK\r\n");

    char buf[16];
    ssize_t n = ::read(client_fd, buf, sizeof(buf));
    TEST_ASSERT(n == 0); // Clean EOF
    ::close(client_fd);
    std::printf("  [PASS] QUIT -> +OK and clean server disconnect\n");

    // Shutdown test server
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);

    std::printf("[TEST] All M5 end-to-end command tests PASSED.\n");
    return 0;
}
