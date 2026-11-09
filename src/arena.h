#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace tinykv {

//=============================================================================
// Arena: Sequential bump allocator for contiguous memory blocks.
// Fast O(1) allocation without individual deallocations.
// Resetting the arena clears all allocations simultaneously.
//=============================================================================
class Arena {
public:
    static constexpr size_t kDefaultChunkSize = 64 * 1024; // 64 KB

    explicit Arena(size_t chunk_size = kDefaultChunkSize)
        : default_chunk_size_(chunk_size) {}

    ~Arena() {
        reset_and_free_all();
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
        : head_chunk_(other.head_chunk_),
          default_chunk_size_(other.default_chunk_size_),
          total_allocated_(other.total_allocated_),
          total_capacity_(other.total_capacity_) {
        other.head_chunk_ = nullptr;
        other.total_allocated_ = 0;
        other.total_capacity_ = 0;
    }

    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            reset_and_free_all();
            head_chunk_ = other.head_chunk_;
            default_chunk_size_ = other.default_chunk_size_;
            total_allocated_ = other.total_allocated_;
            total_capacity_ = other.total_capacity_;
            other.head_chunk_ = nullptr;
            other.total_allocated_ = 0;
            other.total_capacity_ = 0;
        }
        return *this;
    }

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) {
            return nullptr;
        }

        if (head_chunk_ != nullptr) {
            uintptr_t curr_ptr = reinterpret_cast<uintptr_t>(head_chunk_->data + head_chunk_->offset);
            uintptr_t aligned_ptr = (curr_ptr + (alignment - 1)) & ~(alignment - 1);
            size_t padding = static_cast<size_t>(aligned_ptr - curr_ptr);

            if (head_chunk_->offset + padding + bytes <= head_chunk_->capacity) {
                head_chunk_->offset += padding + bytes;
                total_allocated_ += padding + bytes;
                return reinterpret_cast<void*>(aligned_ptr);
            }
        }

        // Need new chunk
        size_t next_chunk_cap = default_chunk_size_;
        if (bytes + alignment > next_chunk_cap) {
            next_chunk_cap = bytes + alignment;
        }

        Chunk* new_chunk = allocate_chunk(next_chunk_cap);
        if (new_chunk == nullptr) {
            return nullptr;
        }

        new_chunk->next = head_chunk_;
        head_chunk_ = new_chunk;

        uintptr_t curr_ptr = reinterpret_cast<uintptr_t>(head_chunk_->data);
        uintptr_t aligned_ptr = (curr_ptr + (alignment - 1)) & ~(alignment - 1);
        size_t padding = static_cast<size_t>(aligned_ptr - curr_ptr);

        head_chunk_->offset = padding + bytes;
        total_allocated_ += padding + bytes;
        return reinterpret_cast<void*>(aligned_ptr);
    }

    void reset() {
        Chunk* curr = head_chunk_;
        while (curr != nullptr) {
            curr->offset = 0;
            curr = curr->next;
        }
        total_allocated_ = 0;
    }

    [[nodiscard]] size_t total_allocated() const noexcept { return total_allocated_; }
    [[nodiscard]] size_t total_capacity() const noexcept { return total_capacity_; }

private:
    struct Chunk {
        Chunk* next{nullptr};
        size_t capacity{0};
        size_t offset{0};
        alignas(std::max_align_t) uint8_t data[1];
    };

    static Chunk* allocate_chunk(size_t capacity) {
        size_t total_bytes = sizeof(Chunk) - 1 + capacity;
        void* mem = std::malloc(total_bytes);
        if (mem == nullptr) {
            return nullptr;
        }
        auto* chunk = static_cast<Chunk*>(mem);
        chunk->next = nullptr;
        chunk->capacity = capacity;
        chunk->offset = 0;
        return chunk;
    }

    void reset_and_free_all() {
        Chunk* curr = head_chunk_;
        while (curr != nullptr) {
            Chunk* next = curr->next;
            std::free(curr);
            curr = next;
        }
        head_chunk_ = nullptr;
        total_allocated_ = 0;
        total_capacity_ = 0;
    }

    Chunk* head_chunk_{nullptr};
    size_t default_chunk_size_{kDefaultChunkSize};
    size_t total_allocated_{0};
    size_t total_capacity_{0};
};

//=============================================================================
// SlabAllocator: Fixed-size bin/class allocator with freelist recycling.
// Ideal for high-churn key-value stores where keys and values of various
// lengths are repeatedly inserted, updated, and deleted.
//=============================================================================
class SlabAllocator {
public:
    static constexpr size_t kSizeClasses[] = {
        16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
    };
    static constexpr size_t kNumSizeClasses = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);
    static constexpr size_t kMaxSlabClassSize = 4096;
    static constexpr size_t kSlabChunkSize = 64 * 1024; // 64 KB

    SlabAllocator() {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            free_lists_[i] = nullptr;
            active_chunk_offsets_[i] = kSlabChunkSize; // forces allocation on first request
        }
    }

    ~SlabAllocator() {
        free_all();
    }

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;

    SlabAllocator(SlabAllocator&& other) noexcept
        : chunk_head_(other.chunk_head_),
          bytes_allocated_(other.bytes_allocated_),
          bytes_reserved_(other.bytes_reserved_),
          num_allocations_(other.num_allocations_),
          num_chunks_(other.num_chunks_) {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            free_lists_[i] = other.free_lists_[i];
            active_chunks_[i] = other.active_chunks_[i];
            active_chunk_offsets_[i] = other.active_chunk_offsets_[i];
            other.free_lists_[i] = nullptr;
            other.active_chunks_[i] = nullptr;
            other.active_chunk_offsets_[i] = kSlabChunkSize;
        }
        other.chunk_head_ = nullptr;
        other.bytes_allocated_ = 0;
        other.bytes_reserved_ = 0;
        other.num_allocations_ = 0;
        other.num_chunks_ = 0;
    }

    SlabAllocator& operator=(SlabAllocator&& other) noexcept {
        if (this != &other) {
            free_all();
            chunk_head_ = other.chunk_head_;
            bytes_allocated_ = other.bytes_allocated_;
            bytes_reserved_ = other.bytes_reserved_;
            num_allocations_ = other.num_allocations_;
            num_chunks_ = other.num_chunks_;
            for (size_t i = 0; i < kNumSizeClasses; ++i) {
                free_lists_[i] = other.free_lists_[i];
                active_chunks_[i] = other.active_chunks_[i];
                active_chunk_offsets_[i] = other.active_chunk_offsets_[i];
                other.free_lists_[i] = nullptr;
                other.active_chunks_[i] = nullptr;
                other.active_chunk_offsets_[i] = kSlabChunkSize;
            }
            other.chunk_head_ = nullptr;
            other.bytes_allocated_ = 0;
            other.bytes_reserved_ = 0;
            other.num_allocations_ = 0;
            other.num_chunks_ = 0;
        }
        return *this;
    }

    // Allocate memory for 'bytes' payload
    void* allocate(size_t bytes) {
        if (bytes == 0) {
            return nullptr;
        }

        if (bytes > kMaxSlabClassSize) {
            void* p = std::malloc(bytes);
            if (p != nullptr) {
                bytes_allocated_ += bytes;
                bytes_reserved_ += bytes;
                ++num_allocations_;
            }
            return p;
        }

        size_t sc_idx = find_size_class(bytes);
        size_t slot_size = kSizeClasses[sc_idx];

        // 1. Check free list
        if (free_lists_[sc_idx] != nullptr) {
            FreeNode* node = free_lists_[sc_idx];
            free_lists_[sc_idx] = node->next;
            bytes_allocated_ += bytes;
            ++num_allocations_;
            return static_cast<void*>(node);
        }

        // 2. Carve from active chunk for this size class
        if (active_chunks_[sc_idx] == nullptr ||
            active_chunk_offsets_[sc_idx] + slot_size > kSlabChunkSize) {
            // Allocate a new 64 KB slab chunk
            SlabChunk* new_chunk = allocate_slab_chunk();
            if (new_chunk == nullptr) {
                return nullptr;
            }
            active_chunks_[sc_idx] = new_chunk;
            active_chunk_offsets_[sc_idx] = 0;
        }

        void* ptr = active_chunks_[sc_idx]->data + active_chunk_offsets_[sc_idx];
        active_chunk_offsets_[sc_idx] += slot_size;
        bytes_allocated_ += bytes;
        ++num_allocations_;
        return ptr;
    }

    // Deallocate memory previously allocated with size 'bytes'
    void deallocate(void* ptr, size_t bytes) noexcept {
        if (ptr == nullptr || bytes == 0) {
            return;
        }

        if (bytes > kMaxSlabClassSize) {
            std::free(ptr);
            bytes_allocated_ -= bytes;
            bytes_reserved_ -= bytes;
            --num_allocations_;
            return;
        }

        size_t sc_idx = find_size_class(bytes);
        auto* node = static_cast<FreeNode*>(ptr);
        node->next = free_lists_[sc_idx];
        free_lists_[sc_idx] = node;

        bytes_allocated_ -= bytes;
        --num_allocations_;
    }

    [[nodiscard]] static size_t find_size_class(size_t bytes) noexcept {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            if (bytes <= kSizeClasses[i]) {
                return i;
            }
        }
        return kNumSizeClasses - 1;
    }

    [[nodiscard]] size_t bytes_allocated() const noexcept { return bytes_allocated_; }
    [[nodiscard]] size_t bytes_reserved() const noexcept { return bytes_reserved_; }
    [[nodiscard]] size_t num_allocations() const noexcept { return num_allocations_; }
    [[nodiscard]] size_t num_chunks() const noexcept { return num_chunks_; }

private:
    struct FreeNode {
        FreeNode* next{nullptr};
    };

    struct SlabChunk {
        SlabChunk* next{nullptr};
        alignas(std::max_align_t) uint8_t data[kSlabChunkSize];
    };

    SlabChunk* allocate_slab_chunk() {
        void* mem = std::malloc(sizeof(SlabChunk));
        if (mem == nullptr) {
            return nullptr;
        }
        auto* chunk = static_cast<SlabChunk*>(mem);
        chunk->next = chunk_head_;
        chunk_head_ = chunk;
        bytes_reserved_ += sizeof(SlabChunk);
        ++num_chunks_;
        return chunk;
    }

    void free_all() noexcept {
        SlabChunk* curr = chunk_head_;
        while (curr != nullptr) {
            SlabChunk* next = curr->next;
            std::free(curr);
            curr = next;
        }
        chunk_head_ = nullptr;
        bytes_allocated_ = 0;
        bytes_reserved_ = 0;
        num_allocations_ = 0;
        num_chunks_ = 0;
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            free_lists_[i] = nullptr;
            active_chunks_[i] = nullptr;
            active_chunk_offsets_[i] = kSlabChunkSize;
        }
    }

    FreeNode* free_lists_[kNumSizeClasses]{nullptr};
    SlabChunk* active_chunks_[kNumSizeClasses]{nullptr};
    size_t active_chunk_offsets_[kNumSizeClasses]{0};

    SlabChunk* chunk_head_{nullptr};
    size_t bytes_allocated_{0};
    size_t bytes_reserved_{0};
    size_t num_allocations_{0};
    size_t num_chunks_{0};
};

} // namespace tinykv
