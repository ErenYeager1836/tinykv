#pragma once

#include <cstdint>
#include <ctime>

namespace tinykv {

// Return current monotonic timestamp in milliseconds using CLOCK_MONOTONIC_COARSE.
// This executes in userspace via VDSO on modern Linux, avoiding system call overhead.
inline uint64_t get_monotonic_time_ms() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

} // namespace tinykv
