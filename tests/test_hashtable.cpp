#include "arena.h"
#include "hashtable.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
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

void test_arena_basic() {
    tinykv::Arena arena(1024); // Small 1 KB chunk size for testing chunk growth

    void* p1 = arena.allocate(100, 8);
    TEST_ASSERT(p1 != nullptr);
    TEST_ASSERT((reinterpret_cast<uintptr_t>(p1) % 8) == 0);

    void* p2 = arena.allocate(200, 16);
    TEST_ASSERT(p2 != nullptr);
    TEST_ASSERT((reinterpret_cast<uintptr_t>(p2) % 16) == 0);

    // Large allocation forcing a new chunk
    void* p3 = arena.allocate(2048, 8);
    TEST_ASSERT(p3 != nullptr);
    TEST_ASSERT(arena.total_allocated() >= 2348);

    // Reset arena
    arena.reset();
    TEST_ASSERT(arena.total_allocated() == 0);

    // Can allocate again after reset
    void* p4 = arena.allocate(50, 8);
    TEST_ASSERT(p4 != nullptr);
    std::printf("  [PASS] test_arena_basic\n");
}

void test_slab_allocator() {
    tinykv::SlabAllocator slab;

    // 1. Small class allocations
    void* a1 = slab.allocate(12); // rounds to 16
    void* a2 = slab.allocate(28); // rounds to 32
    void* a3 = slab.allocate(60); // rounds to 64
    TEST_ASSERT(a1 != nullptr);
    TEST_ASSERT(a2 != nullptr);
    TEST_ASSERT(a3 != nullptr);
    TEST_ASSERT(slab.num_allocations() == 3);

    // 2. Large allocation (> 4096 bytes)
    void* large = slab.allocate(8192);
    TEST_ASSERT(large != nullptr);
    TEST_ASSERT(slab.num_allocations() == 4);

    // 3. Deallocate and verify recycling
    slab.deallocate(a2, 28);
    TEST_ASSERT(slab.num_allocations() == 3);

    // Allocating 32 bytes again should recycle a2
    void* recycled = slab.allocate(30);
    TEST_ASSERT(recycled == a2);
    TEST_ASSERT(slab.num_allocations() == 4);

    slab.deallocate(a1, 12);
    slab.deallocate(recycled, 30);
    slab.deallocate(a3, 60);
    slab.deallocate(large, 8192);
    TEST_ASSERT(slab.num_allocations() == 0);
    TEST_ASSERT(slab.bytes_allocated() == 0);

    // 4. Multi-object churn in slab allocator
    constexpr size_t kItems = 2000;
    std::vector<std::pair<void*, size_t>> allocs;
    allocs.reserve(kItems);

    for (size_t i = 0; i < kItems; ++i) {
        size_t sz = (i % 512) + 1;
        void* ptr = slab.allocate(sz);
        TEST_ASSERT(ptr != nullptr);
        std::memset(ptr, 0xAB, sz);
        allocs.emplace_back(ptr, sz);
    }
    TEST_ASSERT(slab.num_allocations() == kItems);

    // Deallocate in reverse order
    for (auto it = allocs.rbegin(); it != allocs.rend(); ++it) {
        slab.deallocate(it->first, it->second);
    }
    TEST_ASSERT(slab.num_allocations() == 0);
    TEST_ASSERT(slab.bytes_allocated() == 0);

    std::printf("  [PASS] test_slab_allocator\n");
}

void test_hashtable_basic() {
    tinykv::HashTable table(16);
    TEST_ASSERT(table.size() == 0);
    TEST_ASSERT(table.capacity() == 16);
    TEST_ASSERT(table.tombstones() == 0);

    // Lookups on empty table
    TEST_ASSERT(table.get("key1") == nullptr);
    TEST_ASSERT(!table.exists("key1"));

    // Insert
    TEST_ASSERT(table.set("user:100", "Alice"));
    TEST_ASSERT(table.size() == 1);
    TEST_ASSERT(table.exists("user:100"));

    const auto* e = table.get("user:100");
    TEST_ASSERT(e != nullptr);
    TEST_ASSERT(e->key_view() == "user:100");
    TEST_ASSERT(e->val_view() == "Alice");

    // Overwrite existing key
    TEST_ASSERT(table.set("user:100", "Bob"));
    TEST_ASSERT(table.size() == 1);
    e = table.get("user:100");
    TEST_ASSERT(e != nullptr);
    TEST_ASSERT(e->val_view() == "Bob");

    // Multiple keys
    TEST_ASSERT(table.set("user:200", "Charlie"));
    TEST_ASSERT(table.set("user:300", "David"));
    TEST_ASSERT(table.size() == 3);
    TEST_ASSERT(table.get("user:200")->val_view() == "Charlie");
    TEST_ASSERT(table.get("user:300")->val_view() == "David");

    std::printf("  [PASS] test_hashtable_basic\n");
}

void test_hashtable_deletion_and_tombstones() {
    tinykv::HashTable table(16);

    TEST_ASSERT(table.set("k1", "v1"));
    TEST_ASSERT(table.set("k2", "v2"));
    TEST_ASSERT(table.set("k3", "v3"));
    TEST_ASSERT(table.size() == 3);

    // Delete non-existent key
    TEST_ASSERT(!table.del("k_ghost"));

    // Delete existing key
    TEST_ASSERT(table.del("k2"));
    TEST_ASSERT(table.size() == 2);
    TEST_ASSERT(table.tombstones() == 1);
    TEST_ASSERT(table.get("k2") == nullptr);
    TEST_ASSERT(!table.exists("k2"));

    // Other keys still intact
    TEST_ASSERT(table.get("k1")->val_view() == "v1");
    TEST_ASSERT(table.get("k3")->val_view() == "v3");

    // Reinsert deleted key
    TEST_ASSERT(table.set("k2", "v2_new"));
    TEST_ASSERT(table.size() == 3);
    TEST_ASSERT(table.get("k2")->val_view() == "v2_new");

    std::printf("  [PASS] test_hashtable_deletion_and_tombstones\n");
}

void test_hashtable_resize_expansion() {
    // Start with small capacity 16
    tinykv::HashTable table(16);
    TEST_ASSERT(table.capacity() == 16);

    constexpr size_t kTotalKeys = 300;
    for (size_t i = 0; i < kTotalKeys; ++i) {
        std::string k = "key_" + std::to_string(i);
        std::string v = "val_" + std::to_string(i * 10);
        TEST_ASSERT(table.set(k, v));
        TEST_ASSERT(table.load_factor() < 0.72); // Stays below ~70% load
    }

    TEST_ASSERT(table.size() == kTotalKeys);
    TEST_ASSERT(table.capacity() >= 512); // Must have expanded multiple times

    // Verify all keys remain intact and correct after multiple resizes
    for (size_t i = 0; i < kTotalKeys; ++i) {
        std::string k = "key_" + std::to_string(i);
        std::string expected_v = "val_" + std::to_string(i * 10);
        const auto* e = table.get(k);
        TEST_ASSERT(e != nullptr);
        TEST_ASSERT(e->val_view() == expected_v);
    }

    std::printf("  [PASS] test_hashtable_resize_expansion (300 keys across 5+ power-of-two resizes)\n");
}

void test_hashtable_tombstone_purge() {
    tinykv::HashTable table(64);

    // Insert 44 keys (just under the 70% threshold: 44 / 64 = 68.75%)
    for (size_t i = 0; i < 44; ++i) {
        std::string k = "item_" + std::to_string(i);
        TEST_ASSERT(table.set(k, "data"));
    }
    TEST_ASSERT(table.size() == 44);
    TEST_ASSERT(table.capacity() == 64);

    // Delete 35 keys
    for (size_t i = 0; i < 35; ++i) {
        std::string k = "item_" + std::to_string(i);
        TEST_ASSERT(table.del(k));
    }
    TEST_ASSERT(table.size() == 9);
    TEST_ASSERT(table.tombstones() == 35);
    TEST_ASSERT(table.capacity() == 64);

    // Inserting 1 more key reaches (9 + 35 + 1) * 10 = 450 >= 448 (64 * 7)
    // Since size (9) < 35% of 64 and tombstones (35) > 0, rehash purges
    // tombstones in-place without doubling capacity!
    TEST_ASSERT(table.set("trigger_purge_item", "new_data"));

    TEST_ASSERT(table.tombstones() == 0);
    TEST_ASSERT(table.size() == 10);
    TEST_ASSERT(table.capacity() == 64);

    // Verify surviving original items and the new item
    for (size_t i = 35; i < 44; ++i) {
        std::string k = "item_" + std::to_string(i);
        TEST_ASSERT(table.exists(k));
    }
    TEST_ASSERT(table.exists("trigger_purge_item"));

    std::printf("  [PASS] test_hashtable_tombstone_purge (in-place tombstone purge verified)\n");
}

void test_hashtable_heavy_churn() {
    tinykv::HashTable table(32);
    std::unordered_map<std::string, std::string> oracle;

    std::mt19937_64 rng(123456789ULL);
    constexpr size_t kNumOps = 25000;

    for (size_t op = 0; op < kNumOps; ++op) {
        uint32_t action = static_cast<uint32_t>(rng() % 100);
        uint32_t key_id = static_cast<uint32_t>(rng() % 1000);
        std::string key = "k_" + std::to_string(key_id);

        if (action < 50) {
            // 50% Insert / Update
            std::string val = "v_" + std::to_string(rng());
            table.set(key, val);
            oracle[key] = val;
        } else if (action < 80) {
            // 30% Lookup
            const auto* e = table.get(key);
            auto it = oracle.find(key);
            if (it != oracle.end()) {
                TEST_ASSERT(e != nullptr);
                TEST_ASSERT(e->val_view() == it->second);
            } else {
                TEST_ASSERT(e == nullptr);
            }
        } else {
            // 20% Delete
            bool deleted = table.del(key);
            auto it = oracle.find(key);
            if (it != oracle.end()) {
                TEST_ASSERT(deleted == true);
                oracle.erase(it);
            } else {
                TEST_ASSERT(deleted == false);
            }
        }
    }

    // Final verification against oracle
    TEST_ASSERT(table.size() == oracle.size());
    for (const auto& [k, v] : oracle) {
        const auto* e = table.get(k);
        TEST_ASSERT(e != nullptr);
        TEST_ASSERT(e->val_view() == v);
    }

    std::printf("  [PASS] test_hashtable_heavy_churn (25,000 random operations against oracle)\n");
}

void test_hashtable_clear_and_move() {
    tinykv::HashTable t1(16);
    t1.set("alpha", "1");
    t1.set("beta", "2");
    TEST_ASSERT(t1.size() == 2);

    // Move construct
    tinykv::HashTable t2(std::move(t1));
    TEST_ASSERT(t2.size() == 2);
    TEST_ASSERT(t1.size() == 0);
    TEST_ASSERT(t2.get("alpha")->val_view() == "1");

    // Clear
    t2.clear();
    TEST_ASSERT(t2.size() == 0);
    TEST_ASSERT(t2.tombstones() == 0);
    TEST_ASSERT(t2.get("alpha") == nullptr);

    std::printf("  [PASS] test_hashtable_clear_and_move\n");
}

} // namespace

int main() {
    std::printf("[TEST] Running HashTable and Arena unit test suite...\n");
    test_arena_basic();
    test_slab_allocator();
    test_hashtable_basic();
    test_hashtable_deletion_and_tombstones();
    test_hashtable_resize_expansion();
    test_hashtable_tombstone_purge();
    test_hashtable_heavy_churn();
    test_hashtable_clear_and_move();
    std::printf("[TEST] All HashTable & Arena unit tests PASSED.\n");
    return 0;
}
