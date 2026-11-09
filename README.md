# tinykv

A minimal, fast, Redis-compatible key-value server written in modern C++20 for Linux.

> **Project Status**: Complete (Milestones 1–8). Production-ready portfolio project with measured benchmarks, zero compiler warnings, ASAN/UBSAN clean, `-fno-exceptions`, `-fno-rtti`, custom open-addressing hash table and slab allocator, and RESP2 compliance.

---

## What it is

`tinykv` is a learning-scale systems programming project built to explore low-level network programming, Linux `epoll` event loops, protocol parsing, and custom memory management. It speaks the standard Redis Serialization Protocol (RESP2), allowing standard tools like `redis-cli` and `redis-benchmark` to connect without modification.

### Core Goals
- **Minimal binary footprint**: Crushes general-purpose servers on static binary size (35 KB dynamic, 871 KB static standalone vs. 3.2 MB Valkey) and cold startup latency (1.4 ms vs. 46.5 ms Valkey).
- **Single-threaded simplicity**: Pure event-driven epoll loop without locks, race conditions, or context-switch overhead.
- **High throughput pipelining**: Exceeds 1,000,000 req/sec under pipelined workloads (+75% to +110% over Valkey).
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

## Benchmark Results (tinykv vs. Valkey 9.1.2)

Tested using `valkey-benchmark` on Linux `7.2.11-arch1-1` (11th Gen Intel Core i5-1135G7 @ 2.40GHz, 50 concurrent connections, 100,000 requests per run, median of 5 runs). Run via `bench/run.sh`.

| Workload | Pipeline | Value Size | tinykv (req/sec) | Valkey 9.1.2 (req/sec) | tinykv p99 | Valkey p99 | Speedup (%) |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **SET** | P=1 | 16 B | 74,850.30 | 76,745.97 | 0.711 ms | 0.575 ms | -2.5% |
| **GET** | P=1 | 16 B | 74,404.77 | 77,041.60 | 0.751 ms | 0.695 ms | -3.4% |
| **SET** | P=1 | 256 B | 74,515.65 | 78,125.00 | 0.719 ms | 0.743 ms | -4.6% |
| **GET** | P=1 | 256 B | 73,046.02 | 77,041.60 | 0.687 ms | 0.743 ms | -5.2% |
| **SET** | P=16 | 16 B | **970,873.81** | 460,829.50 | **0.615 ms** | 2.175 ms | **+110.7%** |
| **GET** | P=16 | 16 B | **1,020,408.19** | 581,395.31 | **0.639 ms** | 1.807 ms | **+75.5%** |
| **SET** | P=16 | 256 B | **952,381.00** | 460,829.50 | **0.647 ms** | 2.135 ms | **+106.7%** |
| **GET** | P=16 | 256 B | **934,579.44** | 529,100.56 | **0.671 ms** | 2.231 ms | **+76.6%** |

*Note: For non-pipelined ping-pong requests (`P=1`), Valkey edges out tinykv by 2.5% to 5.2%. For pipelined requests (`P=16`), tinykv outperforms Valkey by 75% to 110% due to its zero-copy RESP parser, single-threaded zero-lock architecture, and contiguous slab allocation.*

---

## Binary Size & Startup Time

### Binary Footprint (Disk & Memory)
Measured on Arch Linux x86-64 using GCC 16.2.1 (`-Os`, `-flto`, `-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections`, stripped):

| Target | Binary Size | Text Section | Dependencies | Ratio vs. Valkey |
|:---|:---:|:---:|:---|:---:|
| **Valkey 9.1.2** (`valkey-server`) | **3,276 KB** (3.2 MB) | 2,933,314 B | glibc, libm, libssl, jemalloc, lua | 1.0× (baseline) |
| **tinykv (Default Release)** | **80 KB** | 63,114 B | glibc, libstdc++ | **41× smaller** |
| **tinykv (Dynamic MinSizeRel)** | **35 KB** | 25,723 B | glibc, libstdc++ | **91× smaller** |
| **tinykv (Dynamic libc-only)** | **168 KB** | 151,416 B | glibc only (static libstdc++) | **19× smaller** |
| **tinykv (Fully Static Standalone)** | **871 KB** | 833,706 B | None (zero external shared libs) | **3.7× smaller** |

### Cold Startup Latency (Time to First PING Reply)
Measured using `bench/startup_bench` (spawning cold processes over loopback and timing socket connection + first `+PONG` reply, median of 20 runs):

| Server | Median Startup Latency | Min Latency | Max Latency | Speedup |
|:---|:---:|:---:|:---:|:---:|
| **Valkey 9.1.2** | 46.46 ms | 45.13 ms | 230.47 ms | 1.0× (baseline) |
| **tinykv (Dynamic MinSizeRel)** | **3.34 ms** | 2.97 ms | 4.49 ms | **13.9× faster** |
| **tinykv (Fully Static Standalone)** | **1.40 ms** | 1.24 ms | 1.52 ms | **33.2× faster** |

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

---

## Quick Start

### Requirements
- Arch Linux (or any modern Linux distribution with kernel 5.x+)
- GCC 13+ or Clang 16+ supporting C++20
- CMake 3.20+
- Ninja

### 1. Default Release Build
```bash
# Clone the repository
git clone https://github.com/<your-username>/tinykv.git
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
# Reproducible throughput benchmark against Valkey
./bench/run.sh

# Cold startup time benchmark (median of 20 runs)
./build/startup_bench --runs 20
```

---

## Credits & References

- [Redis](https://github.com/redis/redis) by Salvatore Sanfilippo (antirez) for the RESP2 specification, active expiry algorithm, and architectural inspiration.
- [Valkey](https://github.com/valkey-io/valkey) community for providing the high-performance Linux reference server against which `tinykv` is benchmarked.
- James Smith's *"Build Your Own Redis with C/C++"* tutorials for non-blocking event-loop design patterns.
- Austin Appleby for MurmurHash3.

