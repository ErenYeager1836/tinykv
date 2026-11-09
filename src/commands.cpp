#include "commands.h"

#include <algorithm>
#include <cctype>

namespace tinykv {

namespace {

std::string to_upper(std::string_view sv) {
    std::string s;
    s.reserve(sv.size());
    for (char c : sv) {
        s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return s;
}

} // namespace

ExecutionResult CommandDispatcher::dispatch(const Command& cmd, std::vector<uint8_t>& out_reply) {
    ExecutionResult result{};
    if (cmd.args.empty()) {
        return result;
    }

    std::string command_name = to_upper(cmd.args[0]);

    if (command_name == "PING") {
        handle_ping(cmd, out_reply);
    } else if (command_name == "ECHO") {
        handle_echo(cmd, out_reply);
    } else if (command_name == "SET") {
        handle_set(cmd, out_reply);
    } else if (command_name == "GET") {
        handle_get(cmd, out_reply);
    } else if (command_name == "QUIT") {
        write_simple_string(out_reply, "OK");
        result.close_connection = true;
    } else if (command_name == "COMMAND") {
        // Compatibility handler for redis-cli / redis-benchmark probing commands at startup
        write_empty_array(out_reply);
    } else if (command_name == "CONFIG") {
        // Compatibility handler for CONFIG GET
        write_empty_array(out_reply);
    } else if (command_name == "CLIENT") {
        // Compatibility handler for CLIENT SETINFO / CLIENT LIST
        write_simple_string(out_reply, "OK");
    } else {
        std::string err_msg = "ERR unknown command '" + cmd.args[0] + "'";
        write_error(out_reply, err_msg);
    }

    return result;
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
    if (cmd.args.size() == 3) {
        db_[cmd.args[1]] = cmd.args[2];
        write_simple_string(out, "OK");
    } else {
        write_error(out, "ERR wrong number of arguments for 'set' command");
    }
}

void CommandDispatcher::handle_get(const Command& cmd, std::vector<uint8_t>& out) {
    if (cmd.args.size() == 2) {
        auto it = db_.find(cmd.args[1]);
        if (it != db_.end()) {
            write_bulk_string(out, it->second);
        } else {
            write_null_bulk_string(out);
        }
    } else {
        write_error(out, "ERR wrong number of arguments for 'get' command");
    }
}

} // namespace tinykv
