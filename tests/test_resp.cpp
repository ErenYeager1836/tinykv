#include "resp.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
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

void test_basic_resp_arrays() {
    // 1. PING
    const std::string ping_payload = "*1\r\n$4\r\nPING\r\n";
    size_t consumed = 0;
    tinykv::Command cmd;
    tinykv::ParseStatus status = tinykv::parse_resp(
        reinterpret_cast<const uint8_t*>(ping_payload.data()),
        ping_payload.size(), consumed, cmd);

    TEST_ASSERT(status == tinykv::ParseStatus::kOk);
    TEST_ASSERT(consumed == ping_payload.size());
    TEST_ASSERT(cmd.args.size() == 1);
    TEST_ASSERT(cmd.args[0] == "PING");

    // 2. SET key value
    const std::string set_payload = "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n";
    status = tinykv::parse_resp(
        reinterpret_cast<const uint8_t*>(set_payload.data()),
        set_payload.size(), consumed, cmd);

    TEST_ASSERT(status == tinykv::ParseStatus::kOk);
    TEST_ASSERT(consumed == set_payload.size());
    TEST_ASSERT(cmd.args.size() == 3);
    TEST_ASSERT(cmd.args[0] == "SET");
    TEST_ASSERT(cmd.args[1] == "mykey");
    TEST_ASSERT(cmd.args[2] == "myvalue");

    // 3. Binary safe value with embedded null byte
    std::string bin_payload = "*2\r\n$4\r\nECHO\r\n$6\r\nhe\0llo\r\n";
    // Fix up actual null byte representation in string
    std::string raw_bin = "*2\r\n$4\r\nECHO\r\n$6\r\n";
    raw_bin.append("he\0llo", 6);
    raw_bin.append("\r\n");

    status = tinykv::parse_resp(
        reinterpret_cast<const uint8_t*>(raw_bin.data()),
        raw_bin.size(), consumed, cmd);
    TEST_ASSERT(status == tinykv::ParseStatus::kOk);
    TEST_ASSERT(cmd.args.size() == 2);
    TEST_ASSERT(cmd.args[1].size() == 6);
    TEST_ASSERT(cmd.args[1][2] == '\0');

    std::printf("  [PASS] test_basic_resp_arrays\n");
}

void test_inline_commands() {
    // 1. Simple inline PING
    std::string s1 = "PING\r\n";
    size_t consumed = 0;
    tinykv::Command cmd;
    tinykv::ParseStatus status = tinykv::parse_resp(
        reinterpret_cast<const uint8_t*>(s1.data()), s1.size(), consumed, cmd);
    TEST_ASSERT(status == tinykv::ParseStatus::kOk);
    TEST_ASSERT(consumed == s1.size());
    TEST_ASSERT(cmd.args.size() == 1 && cmd.args[0] == "PING");

    // 2. Multi-argument inline with varied whitespace
    std::string s2 = "  SET   foo   bar123  \r\n";
    status = tinykv::parse_resp(
        reinterpret_cast<const uint8_t*>(s2.data()), s2.size(), consumed, cmd);
    TEST_ASSERT(status == tinykv::ParseStatus::kOk);
    TEST_ASSERT(consumed == s2.size());
    TEST_ASSERT(cmd.args.size() == 3);
    TEST_ASSERT(cmd.args[0] == "SET");
    TEST_ASSERT(cmd.args[1] == "foo");
    TEST_ASSERT(cmd.args[2] == "bar123");

    std::printf("  [PASS] test_inline_commands\n");
}

void test_partial_reads() {
    const std::string full = "*3\r\n$3\r\nSET\r\n$4\r\nuser\r\n$5\r\nalice\r\n";

    // Feed bytes 1-by-1 to verify parser is completely resilient to network packet fragmentation
    std::vector<uint8_t> buffer;
    for (size_t i = 0; i < full.size(); ++i) {
        buffer.push_back(static_cast<uint8_t>(full[i]));
        size_t consumed = 0;
        tinykv::Command cmd;
        tinykv::ParseStatus status = tinykv::parse_resp(buffer.data(), buffer.size(), consumed, cmd);

        if (i + 1 < full.size()) {
            TEST_ASSERT(status == tinykv::ParseStatus::kIncomplete);
            TEST_ASSERT(consumed == 0);
        } else {
            TEST_ASSERT(status == tinykv::ParseStatus::kOk);
            TEST_ASSERT(consumed == full.size());
            TEST_ASSERT(cmd.args.size() == 3);
            TEST_ASSERT(cmd.args[0] == "SET");
            TEST_ASSERT(cmd.args[1] == "user");
            TEST_ASSERT(cmd.args[2] == "alice");
        }
    }

    std::printf("  [PASS] test_partial_reads (1-byte incremental fragmentation)\n");
}

void test_pipelining() {
    // Concatenate 4 commands in a single buffer
    std::string pipeline;
    pipeline += "*1\r\n$4\r\nPING\r\n";
    pipeline += "SET x 100\r\n";
    pipeline += "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n";
    pipeline += "*1\r\n$4\r\nQUIT\r\n";

    std::vector<uint8_t> buf(pipeline.begin(), pipeline.end());
    std::vector<std::string> parsed_commands;

    while (!buf.empty()) {
        size_t consumed = 0;
        tinykv::Command cmd;
        tinykv::ParseStatus status = tinykv::parse_resp(buf.data(), buf.size(), consumed, cmd);
        if (status == tinykv::ParseStatus::kOk) {
            TEST_ASSERT(consumed > 0 && consumed <= buf.size());
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(consumed));
            if (!cmd.args.empty()) {
                parsed_commands.push_back(cmd.args[0]);
            }
        } else {
            TEST_ASSERT(false); // Should not fail or be incomplete
        }
    }

    TEST_ASSERT(parsed_commands.size() == 4);
    TEST_ASSERT(parsed_commands[0] == "PING");
    TEST_ASSERT(parsed_commands[1] == "SET");
    TEST_ASSERT(parsed_commands[2] == "GET");
    TEST_ASSERT(parsed_commands[3] == "QUIT");

    std::printf("  [PASS] test_pipelining (4 pipelined commands in one buffer)\n");
}

void test_malformed_and_bounds() {
    auto expect_error = [](const std::string& input) {
        size_t consumed = 0;
        tinykv::Command cmd;
        tinykv::ParseStatus status = tinykv::parse_resp(
            reinterpret_cast<const uint8_t*>(input.data()), input.size(), consumed, cmd);
        TEST_ASSERT(status == tinykv::ParseStatus::kError);
    };

    // 1. Negative array count
    expect_error("*-1\r\n");
    // 2. Zero array count
    expect_error("*0\r\n");
    // 3. Non-numeric array count
    expect_error("*abc\r\n");
    // 4. Missing bulk identifier (e.g. +OK instead of $len)
    expect_error("*1\r\n+OK\r\n");
    // 5. Negative bulk length
    expect_error("*1\r\n$-5\r\nhello\r\n");
    // 6. Non-numeric bulk length
    expect_error("*1\r\n$xyz\r\nhello\r\n");
    // 7. Giant integer overflow length
    expect_error("*1\r\n$999999999999999999999999999999999999999\r\n");
    // 8. Corrupt CRLF after bulk data
    expect_error("*1\r\n$4\r\nPINGXX");
    // 9. Oversized bulk string exceeding 1MB limit
    expect_error("*1\r\n$1048577\r\n");
    // 10. Oversized args count exceeding 1024 limit
    expect_error("*1025\r\n");

    std::printf("  [PASS] test_malformed_and_bounds (10 malformed scenarios verified)\n");
}

void test_reply_serialization() {
    std::vector<uint8_t> out;

    tinykv::write_simple_string(out, "OK");
    tinykv::write_error(out, "ERR something went wrong");
    tinykv::write_integer(out, 1024);
    tinykv::write_bulk_string(out, "hello");
    tinykv::write_null_bulk_string(out);
    tinykv::write_empty_array(out);
    tinykv::write_array_header(out, 2);

    std::string expected;
    expected += "+OK\r\n";
    expected += "-ERR something went wrong\r\n";
    expected += ":1024\r\n";
    expected += "$5\r\nhello\r\n";
    expected += "$-1\r\n";
    expected += "*0\r\n";
    expected += "*2\r\n";

    std::string actual(out.begin(), out.end());
    TEST_ASSERT(actual == expected);
    std::printf("  [PASS] test_reply_serialization\n");
}

void test_fuzz_random_input() {
    constexpr int kIterations = 10000;
    std::mt19937_64 rng(0x1337BEEF); // Deterministic seed for reproducible fuzzing

    for (int iter = 0; iter < kIterations; ++iter) {
        size_t len = static_cast<size_t>(rng() % 512);
        std::vector<uint8_t> random_bytes(len);
        for (size_t i = 0; i < len; ++i) {
            random_bytes[i] = static_cast<uint8_t>(rng() & 0xFF);
        }

        size_t consumed = 0;
        tinykv::Command cmd;
        // MUST NEVER crash, hang, or invoke undefined behavior on random input
        tinykv::ParseStatus status = tinykv::parse_resp(random_bytes.data(), random_bytes.size(), consumed, cmd);
        (void)status;
        if (status == tinykv::ParseStatus::kOk) {
            TEST_ASSERT(consumed <= random_bytes.size());
        }
    }

    std::printf("  [PASS] test_fuzz_random_input (%d random adversarial runs)\n", kIterations);
}

} // namespace

int main() {
    std::printf("[TEST] Running RESP2 parser and serializer test suite...\n");

    test_basic_resp_arrays();
    test_inline_commands();
    test_partial_reads();
    test_pipelining();
    test_malformed_and_bounds();
    test_reply_serialization();
    test_fuzz_random_input();

    std::printf("[TEST] All RESP2 unit & fuzz tests PASSED.\n");
    return 0;
}
