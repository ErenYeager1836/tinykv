#pragma once

#include "hashtable.h"
#include "resp.h"
#include <string>
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

    [[nodiscard]] HashTable& db() noexcept { return db_; }
    [[nodiscard]] const HashTable& db() const noexcept { return db_; }

    // Run active expiry cycle on the database
    size_t active_expire_step(uint64_t now_ms);

private:
    HashTable db_;

    void handle_ping(const Command& cmd, std::vector<uint8_t>& out);
    void handle_echo(const Command& cmd, std::vector<uint8_t>& out);
    void handle_set(const Command& cmd, std::vector<uint8_t>& out);
    void handle_get(const Command& cmd, std::vector<uint8_t>& out);
    void handle_del(const Command& cmd, std::vector<uint8_t>& out);
    void handle_exists(const Command& cmd, std::vector<uint8_t>& out);
    void handle_incr(const Command& cmd, std::vector<uint8_t>& out);
    void handle_decr(const Command& cmd, std::vector<uint8_t>& out);
    void handle_expire(const Command& cmd, std::vector<uint8_t>& out, bool is_ms);
    void handle_ttl(const Command& cmd, std::vector<uint8_t>& out, bool is_ms);
    void handle_dbsize(const Command& cmd, std::vector<uint8_t>& out);

    void handle_incr_decr(const Command& cmd, std::vector<uint8_t>& out, int64_t delta);
};

} // namespace tinykv
