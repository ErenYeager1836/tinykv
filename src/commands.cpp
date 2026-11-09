#include "commands.h"
#include "clock.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace tinykv {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_int64(std::string_view sv, int64_t& out) {
    if (sv.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (sv[0] == '-') {
        negative = true;
        i = 1;
    } else if (sv[0] == '+') {
        i = 1;
    }
    if (i >= sv.size()) return false;

    uint64_t val = 0;
    for (; i < sv.size(); ++i) {
        char c = sv[i];
        if (c < '0' || c > '9') return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (val > (UINT64_MAX / 10)) return false;
        val *= 10;
        if (val > UINT64_MAX - d) return false;
        val += d;
    }

    if (negative) {
        if (val > static_cast<uint64_t>(INT64_MAX) + 1ULL) return false;
        if (val == static_cast<uint64_t>(INT64_MAX) + 1ULL) {
            out = INT64_MIN;
        } else {
            out = -static_cast<int64_t>(val);
        }
    } else {
        if (val > static_cast<uint64_t>(INT64_MAX)) return false;
        out = static_cast<int64_t>(val);
    }
    return true;
}

} // namespace

ExecutionResult CommandDispatcher::dispatch(const Command& cmd, std::vector<uint8_t>& out_reply) {
    ExecutionResult result{};
    if (cmd.args.empty()) {
        return result;
    }

    std::string_view name = cmd.args[0];

    // Branch prediction optimization: order by frequency (GET/SET first)
    if (iequals(name, "GET")) {
        handle_get(cmd, out_reply);
    } else if (iequals(name, "SET")) {
        handle_set(cmd, out_reply);
    } else if (iequals(name, "PING")) {
        handle_ping(cmd, out_reply);
    } else if (iequals(name, "DEL")) {
        handle_del(cmd, out_reply);
    } else if (iequals(name, "EXISTS")) {
        handle_exists(cmd, out_reply);
    } else if (iequals(name, "INCR")) {
        handle_incr(cmd, out_reply);
    } else if (iequals(name, "DECR")) {
        handle_decr(cmd, out_reply);
    } else if (iequals(name, "ECHO")) {
        handle_echo(cmd, out_reply);
    } else if (iequals(name, "EXPIRE")) {
        handle_expire(cmd, out_reply, false);
    } else if (iequals(name, "PEXPIRE")) {
        handle_expire(cmd, out_reply, true);
    } else if (iequals(name, "TTL")) {
        handle_ttl(cmd, out_reply, false);
    } else if (iequals(name, "PTTL")) {
        handle_ttl(cmd, out_reply, true);
    } else if (iequals(name, "DBSIZE")) {
        handle_dbsize(cmd, out_reply);
    } else if (iequals(name, "QUIT")) {
        write_simple_string(out_reply, "OK");
        result.close_connection = true;
    } else if (iequals(name, "COMMAND")) {
        // Compatibility handler for redis-cli / redis-benchmark probing commands at startup
        write_empty_array(out_reply);
    } else if (iequals(name, "CONFIG")) {
        // Compatibility handler for CONFIG GET
        write_empty_array(out_reply);
    } else if (iequals(name, "CLIENT")) {
        // Compatibility handler for CLIENT SETINFO / CLIENT LIST
        write_simple_string(out_reply, "OK");
    } else {
        std::string err_msg = "ERR unknown command '" + std::string(name) + "'";
        write_error(out_reply, err_msg);
    }

    return result;
}

size_t CommandDispatcher::active_expire_step(uint64_t now_ms) {
    constexpr size_t kSample = 20;
    constexpr size_t kMaxIterations = 10;
    size_t total_expired = 0;

    for (size_t iter = 0; iter < kMaxIterations; ++iter) {
        if (db_.size() == 0) break;
        size_t expired = db_.active_expire_step(now_ms, kSample);
        total_expired += expired;
        if (expired <= kSample / 4) {
            break;
        }
    }
    return total_expired;
}

void CommandDispatcher::handle_ping(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() == 1) {
        write_simple_string(out, "PONG");
    } else if (cmd.args.size() == 2) {
        write_bulk_string(out, cmd.args[1]);
    } else {
        write_error(out, "ERR wrong number of arguments for 'ping' command");
    }
}

void CommandDispatcher::handle_echo(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() == 2) {
        write_bulk_string(out, cmd.args[1]);
    } else {
        write_error(out, "ERR wrong number of arguments for 'echo' command");
    }
}

void CommandDispatcher::handle_set(const Command& cmd, std::vector<uint8_t>& out) {
    uint64_t expire_at_ms = 0;

    if (cmd.args.size() == 3) {
        // Normal SET key value
        if (db_.set(cmd.args[1], cmd.args[2], 0)) {
            write_simple_string(out, "OK");
        } else {
            write_error(out, "ERR failed to store key");
        }
    } else if (cmd.args.size() == 5) {
        // SET key value EX seconds | PX milliseconds
        int64_t t = 0;
        if (!parse_int64(cmd.args[4], t) || t <= 0) {
            write_error(out, "ERR value is not an integer or out of range");
            return;
        }
        uint64_t now_ms = get_monotonic_time_ms();
        if (iequals(cmd.args[3], "EX")) {
            expire_at_ms = now_ms + static_cast<uint64_t>(t) * 1000ULL;
        } else if (iequals(cmd.args[3], "PX")) {
            expire_at_ms = now_ms + static_cast<uint64_t>(t);
        } else {
            write_error(out, "ERR syntax error");
            return;
        }

        if (db_.set(cmd.args[1], cmd.args[2], expire_at_ms)) {
            write_simple_string(out, "OK");
        } else {
            write_error(out, "ERR failed to store key");
        }
    } else {
        write_error(out, "ERR syntax error");
    }
}

void CommandDispatcher::handle_get(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() == 2) {
        uint64_t now_ms = get_monotonic_time_ms();
        const auto* entry = db_.lookup(cmd.args[1], now_ms);
        if (entry != nullptr) {
            write_bulk_string(out, entry->val_view());
        } else {
            write_null_bulk_string(out);
        }
    } else {
        write_error(out, "ERR wrong number of arguments for 'get' command");
    }
}

void CommandDispatcher::handle_del(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() < 2) {
        write_error(out, "ERR wrong number of arguments for 'del' command");
        return;
    }

    uint64_t now_ms = get_monotonic_time_ms();
    int64_t deleted_count = 0;

    for (size_t i = 1; i < cmd.args.size(); ++i) {
        const auto* e = db_.get(cmd.args[i]);
        if (e != nullptr) {
            if (e->expire_at_ms != 0 && e->expire_at_ms <= now_ms) {
                db_.del(cmd.args[i]);
            } else {
                if (db_.del(cmd.args[i])) {
                    ++deleted_count;
                }
            }
        }
    }
    write_integer(out, deleted_count);
}

void CommandDispatcher::handle_exists(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() < 2) {
        write_error(out, "ERR wrong number of arguments for 'exists' command");
        return;
    }

    uint64_t now_ms = get_monotonic_time_ms();
    int64_t count = 0;

    for (size_t i = 1; i < cmd.args.size(); ++i) {
        if (db_.lookup(cmd.args[i], now_ms) != nullptr) {
            ++count;
        }
    }
    write_integer(out, count);
}

void CommandDispatcher::handle_incr(const Command& cmd, std::vector<uint8_t>& out) {
    handle_incr_decr(cmd, out, 1);
}

void CommandDispatcher::handle_decr(const Command& cmd, std::vector<uint8_t>& out) {
    handle_incr_decr(cmd, out, -1);
}

void CommandDispatcher::handle_incr_decr(const Command& cmd, std::vector<uint8_t>& out, int64_t delta) {
    if (cmd.args.size() != 2) {
        std::string cmd_name = delta > 0 ? "incr" : "decr";
        write_error(out, "ERR wrong number of arguments for '" + cmd_name + "' command");
        return;
    }

    uint64_t now_ms = get_monotonic_time_ms();
    Entry* e = db_.lookup(cmd.args[1], now_ms);

    if (e == nullptr) {
        int64_t val = delta;
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(val));
        if (db_.set(cmd.args[1], std::string_view(buf, static_cast<size_t>(len)), 0)) {
            write_integer(out, val);
        } else {
            write_error(out, "ERR failed to store key");
        }
        return;
    }

    int64_t current = 0;
    if (!parse_int64(e->val_view(), current)) {
        write_error(out, "ERR value is not an integer or out of range");
        return;
    }

    if ((delta > 0 && current > INT64_MAX - delta) ||
        (delta < 0 && current < INT64_MIN - delta)) {
        write_error(out, "ERR increment or decrement would overflow");
        return;
    }

    int64_t val = current + delta;
    char buf[32];
    int len = std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(val));
    if (db_.update_value(cmd.args[1], std::string_view(buf, static_cast<size_t>(len)))) {
        write_integer(out, val);
    } else {
        write_error(out, "ERR failed to update key");
    }
}

void CommandDispatcher::handle_expire(const Command& cmd, std::vector<uint8_t>& out, bool is_ms) {
    if (cmd.args.size() != 3) {
        std::string cmd_name = is_ms ? "pexpire" : "expire";
        write_error(out, "ERR wrong number of arguments for '" + cmd_name + "' command");
        return;
    }

    int64_t t = 0;
    if (!parse_int64(cmd.args[2], t)) {
        write_error(out, "ERR value is not an integer or out of range");
        return;
    }

    uint64_t now_ms = get_monotonic_time_ms();
    if (t <= 0) {
        bool existed = (db_.lookup(cmd.args[1], now_ms) != nullptr);
        if (existed) {
            db_.del(cmd.args[1]);
        }
        write_integer(out, existed ? 1 : 0);
        return;
    }

    uint64_t expire_at_ms = is_ms
        ? now_ms + static_cast<uint64_t>(t)
        : now_ms + static_cast<uint64_t>(t) * 1000ULL;

    bool ok = db_.set_expire(cmd.args[1], expire_at_ms, now_ms);
    write_integer(out, ok ? 1 : 0);
}

void CommandDispatcher::handle_ttl(const Command& cmd, std::vector<uint8_t>& out, bool is_ms) {
    if (cmd.args.size() != 2) {
        std::string cmd_name = is_ms ? "pttl" : "ttl";
        write_error(out, "ERR wrong number of arguments for '" + cmd_name + "' command");
        return;
    }

    uint64_t now_ms = get_monotonic_time_ms();
    const Entry* e = db_.lookup(cmd.args[1], now_ms);
    if (e == nullptr) {
        write_integer(out, -2);
        return;
    }

    if (e->expire_at_ms == 0) {
        write_integer(out, -1);
        return;
    }

    int64_t diff_ms = static_cast<int64_t>(e->expire_at_ms - now_ms);
    if (diff_ms < 0) {
        write_integer(out, -2);
        return;
    }

    if (is_ms) {
        write_integer(out, diff_ms);
    } else {
        int64_t ttl_sec = (diff_ms + 999) / 1000;
        write_integer(out, ttl_sec);
    }
}

void CommandDispatcher::handle_dbsize(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() != 1) {
        write_error(out, "ERR wrong number of arguments for 'dbsize' command");
        return;
    }
    write_integer(out, static_cast<int64_t>(db_.size()));
}

} // namespace tinykv
