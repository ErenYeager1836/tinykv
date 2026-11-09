#pragma once

#include "arena.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tinykv {

enum class SlotState : uint8_t {
    Empty = 0,
    Occupied = 1,
    Tombstone = 2
};

struct Entry {
    char* key{nullptr};
    char* val{nullptr};
    uint32_t key_len{0};
    uint32_t val_len{0};
    uint32_t hash{0};
    uint64_t expire_at_ms{0}; // 0 = no expiration (used in M5)
    SlotState state{SlotState::Empty};

    [[nodiscard]] std::string_view key_view() const noexcept {
        return {key, key_len};
    }

    [[nodiscard]] std::string_view val_view() const noexcept {
        return {val, val_len};
    }
};

class HashTable {
public:
    static constexpr size_t kDefaultInitialCapacity = 16;

    explicit HashTable(size_t initial_capacity = kDefaultInitialCapacity);
    ~HashTable();

    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    HashTable(HashTable&& other) noexcept;
    HashTable& operator=(HashTable&& other) noexcept;

    // Insert or update key-value pair.
    bool set(std::string_view key, std::string_view val, uint64_t expire_at_ms = 0);

    // Look up entry by key.
    [[nodiscard]] const Entry* get(std::string_view key) const;
    [[nodiscard]] Entry* get_mut(std::string_view key);

    // Delete entry by key.
    bool del(std::string_view key);

    // Check if key exists.
    [[nodiscard]] bool exists(std::string_view key) const;

    // Clear all entries and reset table to initial state.
    void clear();

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] size_t tombstones() const noexcept { return tombstones_; }
    [[nodiscard]] double load_factor() const noexcept {
        return capacity_ > 0
            ? static_cast<double>(size_ + tombstones_) / static_cast<double>(capacity_)
            : 0.0;
    }

    [[nodiscard]] const SlabAllocator& allocator() const noexcept { return allocator_; }

    [[nodiscard]] const Entry* slot_at(size_t index) const noexcept {
        if (index >= capacity_) return nullptr;
        return &entries_[index];
    }

    static uint32_t hash_key(std::string_view key) noexcept;

private:
    void rehash(size_t new_cap);
    static size_t next_power_of_two(size_t n) noexcept;

    Entry* entries_{nullptr};
    size_t capacity_{0};
    size_t mask_{0};
    size_t size_{0};
    size_t tombstones_{0};
    SlabAllocator allocator_;
};

} // namespace tinykv
