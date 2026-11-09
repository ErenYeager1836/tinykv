#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tinykv {

// Hard limits to treat parser input as hostile and prevent DoS / unbounded allocation
constexpr size_t kMaxArgs = 1024;                // Max arguments per command
constexpr size_t kMaxBulkLen = 1024 * 1024;      // Max bulk string length: 1 MB
constexpr size_t kMaxInlineLen = 64 * 1024;      // Max inline command line: 64 KB

enum class ParseStatus {
    kOk,         // Fully parsed a complete command
    kIncomplete, // Partial packet; need more bytes from network
    kError       // Protocol error or security limit violation; abort connection
};

struct Command {
    std::vector<std::string> args;
};

// Incremental RESP2 Parser
// Inspects [data, data + size]. If a complete command is parsed:
// - sets bytes_consumed to the number of bytes belonging to this command
// - populates out_cmd
// - returns ParseStatus::kOk
ParseStatus parse_resp(const uint8_t* data, size_t size, size_t& bytes_consumed, Command& out_cmd);

// RESP2 Serialization Utilities (appends directly to buffer)
void write_simple_string(std::vector<uint8_t>& out, std::string_view str);
void write_error(std::vector<uint8_t>& out, std::string_view err);
void write_integer(std::vector<uint8_t>& out, int64_t val);
void write_bulk_string(std::vector<uint8_t>& out, std::string_view str);
void write_null_bulk_string(std::vector<uint8_t>& out);
void write_empty_array(std::vector<uint8_t>& out);
void write_array_header(std::vector<uint8_t>& out, size_t count);

} // namespace tinykv
