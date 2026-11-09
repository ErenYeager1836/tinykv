#pragma once

#include "resp.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace tinykv {

struct ExecutionResult {
    bool close_connection{false};
};

class CommandDispatcher {
public:
    CommandDispatcher() = default;

    // Dispatches a parsed command and appends the serialized RESP2 reply directly to out_reply
    ExecutionResult dispatch(const Command& cmd, std::vector<uint8_t>& out_reply);

private:
    // M3 temporary in-memory store; M4 will replace this with custom open-addressing hash table
    std::unordered_map<std::string, std::string> db_;

    void handle_ping(const Command& cmd, std::vector<uint8_t>& out);
    void handle_echo(const Command& cmd, std::vector<uint8_t>& out);
    void handle_set(const Command& cmd, std::vector<uint8_t>& out);
    void handle_get(const Command& cmd, std::vector<uint8_t>& out);
};

} // namespace tinykv
