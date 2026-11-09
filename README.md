# tinykv

[![CI](https://github.com/ErenYeager1836/tinykv/actions/workflows/ci.yml/badge.svg)](https://github.com/ErenYeager1836/tinykv/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey)

A minimal, fast, Redis-compatible key-value server written in modern C++20 for Linux. Single-threaded `epoll` event loop, custom open-addressing hash table, and slab allocator speaking the RESP2 protocol.

---

## Performance & Footprint at a Glance

### Benchmark: tinykv vs. Valkey 9.1.2 (in-memory mode)
Tested with `valkey-benchmark` on Linux `7.2.11-arch1-1` (11th Gen Intel Core i5-1135G7 @ 2.40GHz, 50 concurrent connections, 100,000 requests per run, median of 5 runs). Valkey persistence completely disabled (`--save "" --appendonly no --protected-mode no`). Generated reproducibly via [`bench/run.sh`](bench/run.sh).

| Workload | Pipeline | Value Size | tinykv (req/sec) | Valkey 9.1.2 (req/sec) | tinykv p99 | Valkey p99 | Comparison |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **SET** | P=1 | 16 B | 66,622.25 | 66,445.18 | 0.855 ms | 1.223 ms | **Roughly on par** (+0.3%) |
| **GET** | P=1 | 16 B | 64,850.84 | 67,204.30 | 1.159 ms | 0.975 ms | **Roughly on par** (-3.5%) |
| **SET** | P=1 | 256 B | 67,069.08 | 66,755.67 | 0.839 ms | 0.887 ms | **Roughly on par** (+0.5%) |
| **GET** | P=1 | 256 B | 63,856.96 | 67,521.95 | 0.911 ms | 0.831 ms | **Roughly on par** (-5.4%) |
| **SET** | P=16 | 16 B | **800,000.00** | 386,100.38 | **1.703 ms** | 3.079 ms | **+107.2%** |
| **GET** | P=16 | 16 B | **909,090.94** | 502,512.56 | **1.415 ms** | 3.375 ms | **+80.9%** |
| **SET** | P=16 | 256 B | **826,446.31** | 355,871.91 | **1.191 ms** | 4.335 ms | **+132.2%** |
| **GET** | P=16 | 256 B | **775,193.81** | 480,769.22 | **1.447 ms** | 2.783 ms | **+61.2%** |

> **Context on these numbers**:
> - **At `P=1`**: Both servers are roughly on par (~64k–67k rps). At single-request depth, loopback TCP round-trip latency dominates; gaps of 2–5% are within test noise.
> - **At `P=16`**: `tinykv` achieves 775k–909k rps vs. Valkey's 355k–502k rps (+61% to +132%). These figures approach the throughput limits of `valkey-benchmark` itself on loopback. The relative ratio is meaningful because both servers run against the identical benchmark client, but 800k–900k rps represents client saturation rather than the server's absolute ceiling.
> - **Why tinykv is faster on pipelines**: `tinykv` does **far less per command** than Valkey. It has no ACL checks, no cluster routing, no background stat accumulators, no persistence hooks, and no multi-type dispatch branches. Paired with a zero-copy streaming RESP parser and contiguous slab allocation, pipelined batches process with minimal overhead.

---

### Binary Footprint & Cold Startup

#### Disk & Memory Footprint (Like-for-Like Dynamic Comparison)
Measured on Arch Linux x86-64 with GCC 16.2.1 (`-Os`, `-flto`, `-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections`, `strip`):

| Target | Binary Size | Text Section | Dependencies | Like-for-Like Comparison |
|:---|:---:|:---:|:---|:---:|
| **Valkey 9.1.2** (`valkey-server`) | **3,276 KB** (3.2 MB) | 2,933,314 B | glibc, libm, libssl, jemalloc, lua | Baseline (dynamic) |
| **tinykv (Dynamic MinSizeRel)** | **35 KB** | 25,723 B | glibc, libstdc++ | **91× smaller** (dynamic vs. dynamic)* |
| **tinykv (Default Release -O3)** | **80 KB** | 63,114 B | glibc, libstdc++ | **41× smaller** |
| **tinykv (Static Standalone)** | **871 KB** | 833,706 B | None (bundled glibc) | Self-contained (no external shared libs) |

*\* Note: The 35 KB dynamic binary relies on system-provided `libc.so` and `libstdc++.so`, which already reside in system memory. The 871 KB static build embeds glibc syscalls directly into a standalone zero-dependency executable.*

#### Cold Startup Latency (Time to First PING Reply)
Measured using [`bench/startup_bench.cpp`](bench/startup_bench.cpp) (spawning cold processes over loopback and timing socket connection + first `+PONG` reply, median of 20 runs):

| Server | Median Startup Latency | Min Latency | Max Latency | Speedup |
|:---|:---:|:---:|:---:|:---:|
| **Valkey 9.1.2** | 46.52 ms | 45.54 ms | 85.28 ms | 1.0× (baseline) |
| **tinykv (Dynamic MinSizeRel)** | **3.25 ms** | 3.00 ms | 5.98 ms | **14.3× faster** |
| **tinykv (Static Standalone)** | **1.39 ms** | 1.26 ms | 2.36 ms | **33.5× faster** |

---

## What it is

`tinykv` is a learning-scale systems programming project built to explore low-level network programming, Linux `epoll` event loops, protocol parsing, and custom memory management. It speaks the standard Redis Serialization Protocol (RESP2), allowing standard tools like `redis-cli` and `redis-benchmark` to connect without modification.

### Core Goals
- **Minimal binary footprint**: Crushes general-purpose servers on dynamic binary size (35 KB vs 3.2 MB) and cold startup latency (1.4 ms static vs. 46.5 ms Valkey).
- **Single-threaded simplicity**: Pure event-driven epoll loop without locks, race conditions, or context-switch overhead.
- **High throughput pipelining**: Maximizes throughput under pipelined workloads by minimizing per-command state and eliminating buffer memory moves.
- **Memory safety & robustness**: Built with strict warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`), verified clean under AddressSanitizer and UndefinedBehaviorSanitizer, with hard-bounded buffer limits.

---

## Scope & Limitations

### Supported Commands
- **Connection & Server**: `PING`, `ECHO`, `QUIT`, `DBSIZE`
- **Key-Value Operations**: `SET` (with `EX` / `PX` expiry), `GET`, `DEL`, `EXISTS`
- **Counters**: `INCR`, `DECR`
- **Expiry**: `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`
- **Tooling Compatibility**: Harmless handling for startup inspection commands sent by `redis-cli` and `redis-benchmark` (`COMMAND`, `COMMAND DOCS`, `CONFIG GET`, `CLIENT`).

### Explicitly Out of Scope
`tinykv` is **not a full Redis replacement**. The following features are intentionally out of scope:
- Persistence (RDB snapshots, AOF logs)
- Clustering and Sentinel
- Master/Replica replication
- Multi-threading
- Lua scripting
- Pub/Sub
- Advanced data structures (Sets, Sorted Sets, Hashes, Streams, Bitmaps, HyperLogLogs)

---

## Architecture Summary

```
                      Single-Threaded epoll Architecture
                      
                      +-----------------------------+
                      |    epoll_wait() (Kernel)    |
                      +-----------------------------+
                                     |
              +----------------------+----------------------+
              |                      |                      |
        [listen_fd]             [client_fd 1]          [client_fd N]
         (EPOLLIN)            (EPOLLIN|EPOLLRDHUP)   (EPOLLIN|EPOLLOUT)
              |                      |                      |
        handle_accept()        handle_read()          handle_write()
       - accept4() nonblock   - read() nonblock      - write() nonblock
       - set TCP_NODELAY      - parse RESP2 stream   - flush pending buffer
       - add to epoll table   - dispatch command     - drop EPOLLOUT when done
                                     |
                      +--------------+--------------+
                      |                             |
             [ HashTable Storage ]          [ Memory System ]
             - Open-addressing              - SlabAllocator (15 size classes)
             - Linear probing               - Bump Arena for buffers
             - Cached MurmurHash3           - Zero heap fragmentation
             - Tombstone recycling          - Fast O(1) recycling
                      |
             [ Dual Expiration ]
             - Lazy: on lookup()
             - Active: 10 Hz ring scan
```

- **I/O Multiplexing**: Single-threaded level-triggered Linux `epoll` with conditional `EPOLLOUT` registration (prevents CPU spinning).
- **Stream-Safe RESP2 Parser**: Handles partial reads, batch pipelining, inline telnet commands, and oversized hostile inputs safely.
- **Custom Open-Addressing Hash Table**: Linear probing, power-of-two bitmask indexing, cached 32-bit MurmurHash3, tombstone recycling, and dual-mode expansion/purge rehash. Zero `std::unordered_map`.
- **Memory Management**: Slab allocator with 15 size classes (16 B to 4096 B) and freelist recycling. Zero heap fragmentation under high-churn insert/delete cycles.
- **Dual Expiration**: Instant lazy expiration during lookup + active background ring-buffer sweep at 10 Hz with VDSO `CLOCK_MONOTONIC_COARSE`.

See [`docs/DESIGN.md`](docs/DESIGN.md) for full architectural documentation and trade-off analysis.

---

## Quick Start

### Requirements
- Linux (kernel 5.x+)
- GCC 13+ or Clang 16+ supporting C++20
- CMake 3.20+
- Ninja

### 1. Default Release Build
```bash
# Clone the repository
git clone https://github.com/ErenYeager1836/tinykv.git
cd tinykv

# Configure and compile with Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run test suite
ctest --test-dir build --output-on-failure
```

### 2. Size-Optimized Build (35 KB dynamic)
```bash
cmake -B build-minsize -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-minsize
ls -lh build-minsize/tinykv
```

### 3. Fully Static Standalone Build (871 KB zero-dependency)
```bash
cmake -B build-static -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DENABLE_STATIC=ON
cmake --build build-static
ldd build-static/tinykv  # Output: "not a dynamic executable"
```

### 4. Memory Safety & Sanitizer Verification
```bash
cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

### Running the Server
```bash
./build/tinykv --port 6379 --bind 127.0.0.1 --max-clients 1024
```

In another terminal, connect using standard tools or `redis-cli`:
```bash
# Using redis-cli (or valkey-cli)
redis-cli -p 6379 PING
# Output: PONG

redis-cli -p 6379 SET user:1 Alice EX 60
# Output: OK

redis-cli -p 6379 GET user:1
# Output: "Alice"

redis-cli -p 6379 TTL user:1
# Output: (integer) 60

redis-cli -p 6379 INCR counter
# Output: (integer) 1

# Or interact via raw netcat / telnet (inline protocol support)
ncat 127.0.0.1 6379
PING
+PONG
SET key value
+OK
GET key
$5
value
QUIT
+OK
```

### Running Benchmarks
```bash
# Reproducible throughput benchmark against Valkey (requires valkey-server & valkey-benchmark)
./bench/run.sh

# Cold startup time benchmark (median of 20 runs)
./build/startup_bench build-minsize/tinykv tinykv 16392 20
```

---

## Credits & References

- [Redis](https://github.com/redis/redis) by Salvatore Sanfilippo (antirez) for the RESP2 specification, active expiry algorithm, and architectural inspiration.
- [Valkey](https://github.com/valkey-io/valkey) community for providing the high-performance Linux reference server against which `tinykv` is benchmarked.
- James Smith's *"Build Your Own Redis with C/C++"* tutorials for non-blocking event-loop design patterns.
- Austin Appleby for MurmurHash3.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
