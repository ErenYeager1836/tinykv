# tinykv Architecture & Design Document

`tinykv` is a lightweight, high-performance, single-threaded Redis-compatible key-value server written in modern C++20 for Linux. This document explains the architectural principles, trade-offs, and internal mechanics.

---

## 1. Project Philosophy & Goals

### 1.1 Why build tinykv?
Redis is an industry standard for in-memory caching and message brokering. However, full Redis includes clustering, Lua scripting, pub/sub, persistence modules, multi-threaded I/O helpers, and a broad range of complex data types (Streams, Bitmaps, Geospatial, HyperLogLog).

`tinykv` deliberately limits scope to core key-value primitives (`PING`, `ECHO`, `SET`, `GET`, `DEL`, `EXISTS`, `INCR`, `DECR`, `EXPIRE`, `TTL`, `DBSIZE`, `QUIT`) while preserving full binary-level protocol compatibility with standard Redis clients (`redis-cli`, `redis-benchmark`).

### 1.2 Core Constraints
1. **Single-threaded event loop**: Zero mutexes, zero lock contention, zero cache-line thrashing between CPU cores.
2. **Minimal binary footprint**: Target small static binaries by avoiding heavy third-party runtimes, exceptions (`-fno-exceptions`), and RTTI (`-fno-rtti`).
3. **Robust input parsing**: Treat incoming RESP bytes as hostile. Enforce strict bounded allocation, recursion limits, and stream-safe state machines.
4. **Honest benchmarking**: Every metric must be reproducible on identical hardware with recorded compiler and system versions.

---

## 2. Linux Network Socket Fundamentals (M1 Baseline)

Milestone 1 establishes a blocking TCP server to demonstrate low-level POSIX socket primitives before transitioning to non-blocking epoll in Milestone 2.

```
                  Server Socket Lifecycle (POSIX)
                  
  [ socket() ]   --> Creates an unbound IPv4 TCP socket file descriptor
        |
  [ setsockopt() ]-> SO_REUSEADDR prevents EADDRINUSE during TIME_WAIT
        |
  [ bind() ]     --> Binds socket to requested IP tuple (127.0.0.1:6379)
        |
  [ listen() ]   --> Marks socket passive; sets SOMAXCONN connection backlog
        |
+-----> [ accept() ]   --> Blocks until client connects; returns new connected fd
|           |
|     [ read() ]   <--+ Loop over byte stream until EOF or error
|     [ write() ]  ---+ (TCP has no message boundaries)
|           |
|     [ close() ]  --> Send FIN, terminate connection, release fd
|           |
+-----------+
```

### 2.1 Syscall Mechanics

1. **`socket(AF_INET, SOCK_STREAM, 0)`**:
   - `AF_INET`: Specifies IPv4 protocol family.
   - `SOCK_STREAM`: Requests a sequenced, reliable, two-way connection-oriented byte stream (TCP).
   - `0`: Kernel automatically selects `IPPROTO_TCP`.

2. **`setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))`**:
   - When a TCP connection terminates, the endpoint that initiated the active close remains in `TIME_WAIT` for $2 \times \text{MSL}$ (Maximum Segment Lifetime, typically 30–120s on Linux) to prevent stale packets from corrupting new connections.
   - Without `SO_REUSEADDR`, restarting the server quickly triggers `EADDRINUSE` ("Address already in use"). `SO_REUSEADDR` allows immediate re-binding to the port.

3. **`bind(fd, sockaddr*, socklen)`**:
   - Assigns a local address tuple (`sin_family`, `sin_addr`, `sin_port`) to the socket.
   - Port numbers and IP addresses must be converted to **Network Byte Order** (Big-Endian) using `htons()` and `inet_pton()`.

4. **`listen(fd, backlog)`**:
   - Transitions the socket from active (initiating connections) to passive (accepting connections).
   - The kernel maintains two queues:
     - *Incomplete connection queue* (`SYN_RCVD` state - 3-way handshake in progress).
     - *Completed connection queue* (`ESTABLISHED` state - handshake complete, awaiting `accept()`).
   - `SOMAXCONN` (from `/proc/sys/net/core/somaxconn`) sets the upper limit for the completed queue.

5. **`accept(fd, sockaddr*, socklen*)`**:
   - Dequeues the first connection from the completed queue and creates a brand-new file descriptor (`client_fd`) for data communication.
   - The listening socket `listen_fd` remains open and unchanged.

6. **Stream vs. Packet Semantics**:
   - TCP is a byte stream, **not a datagram protocol**.
   - `write()` does not guarantee that the remote peer's `read()` will receive the exact buffer in a single call. A 100-byte write may arrive as 100 1-byte reads, two 50-byte reads, or bundled with subsequent writes.
   - Sockets must track total bytes transferred and loop until full buffers are written.

---

## 3. Milestone 2: Single-Threaded Non-Blocking epoll Event Loop

In Milestone 2, `tinykv` transitioned from sequential blocking I/O to a high-concurrency, single-threaded Linux `epoll` event loop.

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
     - accept4() in loop    - read() until EAGAIN  - write() until EAGAIN
     - SOCK_NONBLOCK        - append to read_buf   - track write_offset
     - SOCK_CLOEXEC         - check 1MB limit      - remove EPOLLOUT when
     - add to epoll table   - queue response         buffer is drained
```

### 3.1 Level-Triggered Event Notification Mechanics
1. **Conditional `EPOLLOUT` Registration**:
   - In level-triggered epoll, a socket send buffer is almost always writable.
   - If `EPOLLOUT` is permanently registered on a client socket, `epoll_wait()` returns immediately every single cycle, spinning the CPU at 100% utilization.
   - **tinykv rule**: Sockets register interest in `EPOLLIN | EPOLLRDHUP` by default. `EPOLLOUT` is added (`epoll_ctl(MOD)`) **only** when a `write()` call returns `EAGAIN` / `EWOULDBLOCK` (indicating kernel socket send buffers are saturated). As soon as the pending write buffer is fully flushed, `EPOLLOUT` is cleared immediately.

2. **Draining Queues with Non-Blocking Loops**:
   - **Accept Loop**: `handle_accept()` calls `accept4(listen_fd, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)` in a loop until `EAGAIN` / `EWOULDBLOCK`, ensuring all simultaneous incoming connection bursts are ingested without latency.
   - **Read Loop**: `handle_read(fd)` drains all available bytes into the connection's `read_buf` until `read()` returns `EAGAIN`.

3. **Fast Disconnect Detection (`EPOLLRDHUP`)**:
   - Registered on Linux to detect when a remote peer closes their write-side of the connection (`SHUT_WR` / FIN packet) directly from the event loop, avoiding wasted read attempts.

4. **Resource Bounds & DoS Resistance**:
   - `kMaxClientBufferSize` (1 MB) limits the dynamic growth of per-connection buffers. A client that streams unbounded data without reading replies is abruptly disconnected before causing out-of-memory (OOM) kernel panics.
   - `config.max_clients` rejects new connections beyond the user's limit before allocating connection state.

---

## 4. Milestone 3: RESP2 Protocol Engine & Command Dispatcher

In Milestone 3, the raw byte echo logic is replaced with a robust, incremental Redis Serialization Protocol (RESP2) parser, reply serializer, and command dispatcher.

```
                  Milestone 3 Request/Response Pipeline
                  
  [ Client Data ]  --> [ Connection read_buf ]
                             |
                      [ parse_resp() ]
                             |
             +---------------+---------------+
             |                               |
      [ RESP Array ]                  [ Inline Format ]
     *<count>\r\n$<len>...            PING\r\n (telnet/cli)
             |                               |
             +---------------+---------------+
                             |
                  [ Command Arguments ]
                 (std::vector<string_view>)
                             |
                  [ dispatch_command() ]
                             |
        +--------+--------+--------+--------+--------+
        |        |        |        |        |        |
      PING     ECHO      SET      GET     QUIT   COMPAT (COMMAND/CONFIG)
        |        |        |        |        |        |
        +--------+--------+--------+--------+--------+
                             |
                     [ Reply Writer ]
             (+OK\r\n, $5\r\nhello\r\n, etc.)
                             |
                [ Connection write_buf ] --> [ Client Socket ]
```

### 4.1 Parser Architecture & Invariants

1. **Incremental Stream Parsing (`parse_resp`)**:
   - Because TCP arrives in arbitrary chunk sizes, the parser operates non-destructively on `std::string_view` slices of `conn.read_buf`.
   - Returns three explicit states:
     - `ParseResult::Done`: A complete command was parsed. `bytes_consumed` indicates the exact number of bytes to remove from `read_buf`.
     - `ParseResult::NeedMoreData`: The buffer contains a partial RESP frame. The parser leaves `read_buf` intact and waits for subsequent `read()` events.
     - `ParseResult::Error`: The client sent a malformed frame, negative length, integer overflow, or exceeded safety thresholds. Returns an error message for an immediate `-ERR` reply, followed by connection termination.

2. **Inline and Array Protocol Support**:
   - Standard Redis clients (`redis-cli`, `redis-benchmark`) emit RESP Arrays (`*<num-args>\r\n$<arg1-len>\r\n<arg1>...`).
   - Debugging tools like `telnet` and `nc` emit plain whitespace-separated inline strings (`PING\r\n`, `GET mykey\r\n`).
   - `tinykv` inspects the initial byte: `*` triggers standard RESP array parsing; any other non-whitespace character triggers the inline command parser.

3. **Adversarial Input & DoS Protections**:
   - **`kMaxArgs = 1024`**: Prevents unbounded dynamic array vector allocation.
   - **`kMaxBulkLen = 1 MB`**: Immediately rejects bulk string headers requesting gigabytes of payload.
   - **`kMaxInlineLen = 64 KB`**: Limits line length for inline commands, preventing endless scans for `\r\n`.
   - **64-bit Integer Overflow Check**: Incremental parsing of integers checks against `INT64_MAX / 10` before multiplication, preventing undefined behavior on wrapped negative lengths.

### 4.2 Reply Serialization
Responses are constructed directly into the client's output buffer using compact RESP2 primitives:
- Simple Strings: `+<str>\r\n` (e.g., `+PONG\r\n`, `+OK\r\n`)
- Errors: `-<err_msg>\r\n` (e.g., `-ERR unknown command\r\n`)
- Integers: `:<int>\r\n`
- Bulk Strings: `$<length>\r\n<binary-safe-bytes>\r\n`
- Null Bulk Strings: `$-1\r\n` (for cache misses on `GET`)
- Empty Arrays: `*0\r\n` (for `COMMAND` and `CONFIG GET` stubs)

### 4.3 Command Execution & Compatibility
- **Case-Insensitive Dispatch**: Command names are normalized to uppercase for dispatch (`ping`, `Ping`, `PING`).
- **Initial Core Commands**:
  - `PING [message]`: Returns `+PONG\r\n` or the echoed message as bulk string.
  - `ECHO <message>`: Echoes the argument as bulk string.
  - `SET <key> <val>`: Stores key-value pair and returns `+OK\r\n`.
  - `GET <key>`: Returns `$len\r\nval\r\n` or `$-1\r\n` if not found.
  - `QUIT`: Returns `+OK\r\n` and flags the connection to close gracefully after the reply is flushed.
- **Client Handshake Compatibility**: `redis-cli` and `redis-benchmark` transmit `COMMAND`, `COMMAND DOCS`, `CONFIG GET`, and `CLIENT` commands on connection startup. `tinykv` handles these safely by returning valid empty array/null responses rather than returning syntax errors, allowing real redis tooling to connect and execute immediately.

