# tinykv

A minimal, fast, Redis-compatible key-value server written in modern C++20 for Linux.

> **Project Status**: Milestone 5 complete (Full portfolio command set: PING, ECHO, SET (with EX/PX), GET, DEL, EXISTS, INCR, DECR, EXPIRE, PEXPIRE, TTL, PTTL, DBSIZE, QUIT; dual lazy and active expiration; pipelining verified).

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

## Quick Start (Milestone 1)

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
