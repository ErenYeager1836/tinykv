# tinykv

A minimal, fast, Redis-compatible key-value server written in modern C++20 for Linux.

> **Project Status**: Milestone 7 complete (Size builds: 35 KB dynamic, 871 KB static standalone; 1.4 ms cold startup time—33× faster than Valkey; -Os, LTO, -fno-exceptions, -fno-rtti, GC sections, stripped).

---

## What it is

`tinykv` is a learning-scale systems programming project built to explore low-level network programming, Linux `epoll` event loops, protocol parsing, and custom memory management. It speaks the standard Redis Serialization Protocol (RESP2), allowing standard tools like `redis-cli` and `redis-benchmark` to connect without modification.

### Core Goals
- **Minimal binary footprint**: Crushes general-purpose servers on static binary size and startup time.
- **Single-threaded simplicity**: Pure event-driven epoll loop without locks, race conditions, or context-switch overhead.
- **Memory safety & robustness**: Built with strict warnings (`-Wall -Wextra -Wpedantic -Werror`), tested with ASAN/UBSAN, with bounded memory allocations.

---

## Scope & Limitations

### Supported Commands (Milestone Target)
- **Connection & Server**: `PING`, `ECHO`, `QUIT`, `DBSIZE`
- **Key-Value Operations**: `SET` (with `EX` / `PX` expiry), `GET`, `DEL`, `EXISTS`
- **Counters**: `INCR`, `DECR`
- **Expiry**: `EXPIRE`, `TTL`
- **Tooling Compatibility**: Harmless handling for startup commands sent by `redis-cli` and `redis-benchmark` (`COMMAND`, `CONFIG GET`).

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

*Note: For non-pipelined ping-pong requests (`P=1`), Valkey edges out tinykv by 2.5% to 5.2%. For pipelined requests (`P=16`), tinykv outperforms Valkey by 75% to 110% due to its zero-copy RESP parser and single-threaded slab allocation.*

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

## Quick Start

### Requirements
- Arch Linux (or any Linux with kernel 5.x+)
- GCC 13+ or Clang 16+ supporting C++20
- CMake 3.20+
- Ninja

### Building
```bash
# Clone the repository
git clone https://github.com/<your-username>/tinykv.git
cd tinykv

# Configure and compile with Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure
```

### Running the Server
```bash
./build/tinykv --port 6379 --bind 127.0.0.1
```

In another terminal, connect using standard tools or `redis-cli`:
```bash
# Using redis-cli (or valkey-cli)
redis-cli -p 6379 PING
# Output: PONG

redis-cli -p 6379 SET user:1 Alice
# Output: OK

redis-cli -p 6379 GET user:1
# Output: "Alice"

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

---

## Design & Architecture

See [docs/DESIGN.md](docs/DESIGN.md) for low-level socket architecture, POSIX syscall mechanics, and the roadmap toward an epoll-driven event loop.

---

## Credits & References

- [Redis](https://github.com/redis/redis) by Salvatore Sanfilippo (antirez) for the RESP2 specification and architecture inspiration.
- James Smith's *"Build Your Own Redis with C/C++"* tutorials for non-blocking event-loop design patterns.
