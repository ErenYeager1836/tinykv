#include "hashtable.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace tinykv {

namespace {

// MurmurHash3 (32-bit yield) - standard fast hash with excellent avalanche properties.
uint32_t murmurhash3_32(const void* key, size_t len, uint32_t seed) noexcept {
    const auto* data = static_cast<const uint8_t*>(key);
    const size_t nblocks = len / 4;
    uint32_t h1 = seed;

    constexpr uint32_t c1 = 0xcc9e2d51U;
    constexpr uint32_t c2 = 0x1b873593U;

    // Body
    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k1 = 0;
        std::memcpy(&k1, data + i * 4, sizeof(uint32_t));

        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> (32 - 15));
        k1 *= c2;

        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> (32 - 13));
        h1 = h1 * 5 + 0xe6546b64U;
    }

    // Tail
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;

    switch (len & 3) {
    case 3:
        k1 ^= static_cast<uint32_t>(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k1 ^= static_cast<uint32_t>(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k1 ^= static_cast<uint32_t>(tail[0]);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> (32 - 15));
        k1 *= c2;
        h1 ^= k1;
        break;
    default:
        break;
    }

    // Finalization
    h1 ^= static_cast<uint32_t>(len);
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6bU;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35U;
    h1 ^= h1 >> 16;

    return h1;
}

} // namespace

size_t HashTable::next_power_of_two(size_t n) noexcept {
    if (n <= 16) {
        return 16;
    }
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

uint32_t HashTable::hash_key(std::string_view key) noexcept {
    constexpr uint32_t kSeed = 0x9747b28cU;
    return murmurhash3_32(key.data(), key.size(), kSeed);
}

HashTable::HashTable(size_t initial_capacity) {
    capacity_ = next_power_of_two(initial_capacity);
    mask_ = capacity_ - 1;
    entries_ = static_cast<Entry*>(std::calloc(capacity_, sizeof(Entry)));
}

HashTable::~HashTable() {
    clear();
    std::free(entries_);
    entries_ = nullptr;
}

HashTable::HashTable(HashTable&& other) noexcept
    : entries_(other.entries_),
      capacity_(other.capacity_),
      mask_(other.mask_),
      size_(other.size_),
      tombstones_(other.tombstones_),
      allocator_(std::move(other.allocator_)) {
    other.entries_ = nullptr;
    other.capacity_ = 0;
    other.mask_ = 0;
    other.size_ = 0;
    other.tombstones_ = 0;
}

HashTable& HashTable::operator=(HashTable&& other) noexcept {
    if (this != &other) {
        clear();
        std::free(entries_);

        entries_ = other.entries_;
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        size_ = other.size_;
        tombstones_ = other.tombstones_;
        allocator_ = std::move(other.allocator_);

        other.entries_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
        other.size_ = 0;
        other.tombstones_ = 0;
    }
    return *this;
}

void HashTable::rehash(size_t new_cap) {
    auto* new_entries = static_cast<Entry*>(std::calloc(new_cap, sizeof(Entry)));
    if (new_entries == nullptr) {
        return;
    }

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < capacity_; ++i) {
        if (entries_[i].state == SlotState::Occupied) {
            uint32_t h = entries_[i].hash;
            size_t idx = h & new_mask;
            for (size_t step = 0; step < new_cap; ++step) {
                size_t slot = (idx + step) & new_mask;
                if (new_entries[slot].state == SlotState::Empty) {
                    new_entries[slot] = entries_[i];
                    break;
                }
            }
        }
    }

    std::free(entries_);
    entries_ = new_entries;
    capacity_ = new_cap;
    mask_ = new_mask;
    tombstones_ = 0;
}

bool HashTable::set(std::string_view key, std::string_view val, uint64_t expire_at_ms) {
    if (key.empty()) {
        return false;
    }

    // Check load factor before insertion: resize if (size_ + tombstones_ + 1) / capacity_ >= 0.70
    if ((size_ + tombstones_ + 1) * 10 >= capacity_ * 7) {
        if (tombstones_ > 0 && size_ * 100 < capacity_ * 35) {
            // High tombstone density: purge tombstones without expanding table
            rehash(capacity_);
        } else {
            // Normal table expansion
            rehash(capacity_ * 2);
        }
    }

    uint32_t h = hash_key(key);
    size_t idx = h & mask_;
    size_t first_tombstone = static_cast<size_t>(-1);
    size_t target_slot = static_cast<size_t>(-1);

    for (size_t step = 0; step < capacity_; ++step) {
        size_t slot = (idx + step) & mask_;
        if (entries_[slot].state == SlotState::Occupied) {
            if (entries_[slot].hash == h &&
                entries_[slot].key_len == key.size() &&
                std::memcmp(entries_[slot].key, key.data(), key.size()) == 0) {
                target_slot = slot;
                break;
            }
        } else if (entries_[slot].state == SlotState::Tombstone) {
            if (first_tombstone == static_cast<size_t>(-1)) {
                first_tombstone = slot;
            }
        } else if (entries_[slot].state == SlotState::Empty) {
            target_slot = (first_tombstone != static_cast<size_t>(-1)) ? first_tombstone : slot;
            break;
        }
    }

    if (target_slot == static_cast<size_t>(-1)) {
        return false;
    }

    if (entries_[target_slot].state == SlotState::Occupied) {
        // Overwriting existing key's value
        allocator_.deallocate(entries_[target_slot].val, entries_[target_slot].val_len + 1);
        char* new_val = static_cast<char*>(allocator_.allocate(val.size() + 1));
        if (new_val == nullptr) {
            return false;
        }
        std::memcpy(new_val, val.data(), val.size());
        new_val[val.size()] = '\0';

        entries_[target_slot].val = new_val;
        entries_[target_slot].val_len = static_cast<uint32_t>(val.size());
        entries_[target_slot].expire_at_ms = expire_at_ms;
        return true;
    }

    // New insertion (Empty or recycled Tombstone)
    char* new_key = static_cast<char*>(allocator_.allocate(key.size() + 1));
    if (new_key == nullptr) {
        return false;
    }
    std::memcpy(new_key, key.data(), key.size());
    new_key[key.size()] = '\0';

    char* new_val = static_cast<char*>(allocator_.allocate(val.size() + 1));
    if (new_val == nullptr) {
        allocator_.deallocate(new_key, key.size() + 1);
        return false;
    }
    std::memcpy(new_val, val.data(), val.size());
    new_val[val.size()] = '\0';

    if (entries_[target_slot].state == SlotState::Tombstone) {
        --tombstones_;
    }

    entries_[target_slot].key = new_key;
    entries_[target_slot].key_len = static_cast<uint32_t>(key.size());
    entries_[target_slot].val = new_val;
    entries_[target_slot].val_len = static_cast<uint32_t>(val.size());
    entries_[target_slot].hash = h;
    entries_[target_slot].expire_at_ms = expire_at_ms;
    entries_[target_slot].state = SlotState::Occupied;
    ++size_;

    return true;
}

const Entry* HashTable::get(std::string_view key) const {
    if (key.empty() || size_ == 0) {
        return nullptr;
    }

    uint32_t h = hash_key(key);
    size_t idx = h & mask_;

    for (size_t step = 0; step < capacity_; ++step) {
        size_t slot = (idx + step) & mask_;
        if (entries_[slot].state == SlotState::Empty) {
            return nullptr;
        }
        if (entries_[slot].state == SlotState::Occupied) {
            if (entries_[slot].hash == h &&
                entries_[slot].key_len == key.size() &&
                std::memcmp(entries_[slot].key, key.data(), key.size()) == 0) {
                return &entries_[slot];
            }
        }
    }
    return nullptr;
}

Entry* HashTable::get_mut(std::string_view key) {
    if (key.empty() || size_ == 0) {
        return nullptr;
    }

    uint32_t h = hash_key(key);
    size_t idx = h & mask_;

    for (size_t step = 0; step < capacity_; ++step) {
        size_t slot = (idx + step) & mask_;
        if (entries_[slot].state == SlotState::Empty) {
            return nullptr;
        }
        if (entries_[slot].state == SlotState::Occupied) {
            if (entries_[slot].hash == h &&
                entries_[slot].key_len == key.size() &&
                std::memcmp(entries_[slot].key, key.data(), key.size()) == 0) {
                return &entries_[slot];
            }
        }
    }
    return nullptr;
}

bool HashTable::del(std::string_view key) {
    if (key.empty() || size_ == 0) {
        return false;
    }

    uint32_t h = hash_key(key);
    size_t idx = h & mask_;

    for (size_t step = 0; step < capacity_; ++step) {
        size_t slot = (idx + step) & mask_;
        if (entries_[slot].state == SlotState::Empty) {
            return false;
        }
        if (entries_[slot].state == SlotState::Occupied) {
            if (entries_[slot].hash == h &&
                entries_[slot].key_len == key.size() &&
                std::memcmp(entries_[slot].key, key.data(), key.size()) == 0) {
                allocator_.deallocate(entries_[slot].key, entries_[slot].key_len + 1);
                allocator_.deallocate(entries_[slot].val, entries_[slot].val_len + 1);
                entries_[slot].key = nullptr;
                entries_[slot].val = nullptr;
                entries_[slot].key_len = 0;
                entries_[slot].val_len = 0;
                entries_[slot].hash = 0;
                entries_[slot].expire_at_ms = 0;
                entries_[slot].state = SlotState::Tombstone;
                --size_;
                ++tombstones_;
                return true;
            }
        }
    }
    return false;
}

bool HashTable::exists(std::string_view key) const {
    return get(key) != nullptr;
}

void HashTable::clear() {
    if (entries_ == nullptr) {
        return;
    }
    for (size_t i = 0; i < capacity_; ++i) {
        if (entries_[i].state == SlotState::Occupied) {
            allocator_.deallocate(entries_[i].key, entries_[i].key_len + 1);
            allocator_.deallocate(entries_[i].val, entries_[i].val_len + 1);
        }
        entries_[i] = Entry{};
    }
    size_ = 0;
    tombstones_ = 0;
}

} // namespace tinykv
