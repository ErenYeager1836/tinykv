#include "resp.h"

#include <cctype>
#include <climits>
#include <cstring>

namespace tinykv {

namespace {

// Locate "\r\n" within [data, data + size].
// Returns index of '\r', or size if not found.
size_t find_crlf(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 1 < size; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return i;
        }
    }
    return size;
}

// Strictly parse signed 64-bit integer with overflow detection
bool parse_int64(const uint8_t* data, size_t len, int64_t& out) {
    if (len == 0) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (data[0] == '-') {
        negative = true;
        i = 1;
        if (len == 1) {
            return false;
        }
    } else if (data[0] == '+') {
        i = 1;
        if (len == 1) {
            return false;
        }
    }

    int64_t val = 0;
    for (; i < len; ++i) {
        if (data[i] < '0' || data[i] > '9') {
            return false;
        }
        int digit = data[i] - '0';
        if (val > (INT64_MAX - digit) / 10) {
            return false; // Integer overflow
        }
        val = val * 10 + digit;
    }

    out = negative ? -val : val;
    return true;
}

// Append an ASCII string representation of integer to a byte buffer
void append_int(std::vector<uint8_t>& out, int64_t val) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(val));
    if (n > 0) {
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(buf),
                   reinterpret_cast<const uint8_t*>(buf) + n);
    }
}

void append_bytes(std::vector<uint8_t>& out, std::string_view sv) {
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(sv.data()),
               reinterpret_cast<const uint8_t*>(sv.data()) + sv.size());
}

} // namespace

ParseStatus parse_resp(const uint8_t* data, size_t size, size_t& bytes_consumed, Command& out_cmd) {
    bytes_consumed = 0;
    if (size == 0) {
        return ParseStatus::kIncomplete;
    }

    // 1. Standard RESP Array of Bulk Strings: "*<count>\r\n$<len>\r\n<data>\r\n..."
    if (data[0] == '*') {
        size_t crlf_pos = find_crlf(data, size);
        if (crlf_pos == size) {
            return ParseStatus::kIncomplete;
        }

        int64_t num_args = 0;
        if (!parse_int64(data + 1, crlf_pos - 1, num_args)) {
            return ParseStatus::kError;
        }

        if (num_args <= 0 || static_cast<size_t>(num_args) > kMaxArgs) {
            return ParseStatus::kError;
        }

        size_t offset = crlf_pos + 2; // Move past "*<num_args>\r\n"
        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(num_args));

        for (int64_t i = 0; i < num_args; ++i) {
            if (offset >= size) {
                return ParseStatus::kIncomplete;
            }

            if (data[offset] != '$') {
                return ParseStatus::kError;
            }

            size_t bulk_crlf = find_crlf(data + offset, size - offset);
            if (bulk_crlf == size - offset) {
                return ParseStatus::kIncomplete;
            }

            int64_t bulk_len = 0;
            if (!parse_int64(data + offset + 1, bulk_crlf - 1, bulk_len)) {
                return ParseStatus::kError;
            }

            if (bulk_len < 0 || static_cast<size_t>(bulk_len) > kMaxBulkLen) {
                return ParseStatus::kError;
            }

            size_t data_start = offset + bulk_crlf + 2;
            size_t total_needed = data_start + static_cast<size_t>(bulk_len) + 2;
            if (total_needed > size) {
                return ParseStatus::kIncomplete;
            }

            // Verify trailing CRLF after bulk data
            size_t data_end = data_start + static_cast<size_t>(bulk_len);
            if (data[data_end] != '\r' || data[data_end + 1] != '\n') {
                return ParseStatus::kError;
            }

            args.emplace_back(reinterpret_cast<const char*>(data + data_start),
                              static_cast<size_t>(bulk_len));

            offset = total_needed;
        }

        bytes_consumed = offset;
        out_cmd.args = std::move(args);
        return ParseStatus::kOk;
    }

    // 2. Inline Command (e.g. "PING\r\n" or "SET foo bar\r\n")
    // Useful for telnet, netcat, or raw testing scripts
    size_t crlf_pos = find_crlf(data, size);
    if (crlf_pos == size) {
        if (size > kMaxInlineLen) {
            return ParseStatus::kError; // Exceeded inline limit
        }
        return ParseStatus::kIncomplete;
    }

    std::string_view line(reinterpret_cast<const char*>(data), crlf_pos);
    std::vector<std::string> args;

    size_t i = 0;
    while (i < line.size()) {
        // Skip whitespace
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }

        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }

        if (args.size() >= kMaxArgs) {
            return ParseStatus::kError;
        }
        args.emplace_back(line.substr(start, i - start));
    }

    bytes_consumed = crlf_pos + 2;
    if (args.empty()) {
        // Empty line (just CRLF) - consume and return incomplete/ok
        out_cmd.args.clear();
        return ParseStatus::kOk;
    }

    out_cmd.args = std::move(args);
    return ParseStatus::kOk;
}

void write_simple_string(std::vector<uint8_t>& out, std::string_view str) {
    out.push_back('+');
    append_bytes(out, str);
    out.push_back('\r');
    out.push_back('\n');
}

void write_error(std::vector<uint8_t>& out, std::string_view err) {
    out.push_back('-');
    append_bytes(out, err);
    out.push_back('\r');
    out.push_back('\n');
}

void write_integer(std::vector<uint8_t>& out, int64_t val) {
    out.push_back(':');
    append_int(out, val);
    out.push_back('\r');
    out.push_back('\n');
}

void write_bulk_string(std::vector<uint8_t>& out, std::string_view str) {
    out.push_back('$');
    append_int(out, static_cast<int64_t>(str.size()));
    out.push_back('\r');
    out.push_back('\n');
    append_bytes(out, str);
    out.push_back('\r');
    out.push_back('\n');
}

void write_null_bulk_string(std::vector<uint8_t>& out) {
    out.push_back('$');
    out.push_back('-');
    out.push_back('1');
    out.push_back('\r');
    out.push_back('\n');
}

void write_empty_array(std::vector<uint8_t>& out) {
    out.push_back('*');
    out.push_back('0');
    out.push_back('\r');
    out.push_back('\n');
}

void write_array_header(std::vector<uint8_t>& out, size_t count) {
    out.push_back('*');
    append_int(out, static_cast<int64_t>(count));
    out.push_back('\r');
    out.push_back('\n');
}

} // namespace tinykv
