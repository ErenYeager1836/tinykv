# tinykv vs Valkey Benchmark Results

## Environment & System Info
- **CPU Model**: `11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz`
- **Linux Kernel**: `7.2.11-arch1-1`
- **Compiler**: `g++ (GCC) 16.2.1 20260810`
- **tinykv Version**: `v0.1.0 (Release)`
- **Comparison Server**: `Valkey server v=9.1.2 sha=7f1dffed:1 malloc=jemalloc-5.3.1 bits=64 build=ab3e536b46916906`
- **Benchmark Tool**: `valkey-benchmark 9.1.2 (git:7f1dffed-dirty)`
- **Valkey Server Flags**: `--save "" --appendonly no --protected-mode no` (persistence completely disabled for fair in-memory comparison)
- **tinykv Server Flags**: `--port 16379 --bind 127.0.0.1` (single-threaded epoll, default slab/arena allocator)
- **Parameters**: `-n 100000`, `-c 50`, 5 runs per configuration (reporting median)

## Performance Comparison Table

| Workload | Pipeline | Value Size | tinykv (req/sec) | Valkey (req/sec) | tinykv p99 (ms) | Valkey p99 (ms) | Speedup (%) |
|:---------|:--------:|:----------:|:----------------:|:-----------------------------:|:---------------:|:----------------------------:|:-----------:|
| SET | P=1 | 16 B | 66622.25 | 66445.18 | 0.855 ms | 1.223 ms | +0.3% |
| GET | P=1 | 16 B | 64850.84 | 67204.30 | 1.159 ms | 0.975 ms | -3.5% |
| SET | P=1 | 256 B | 67069.08 | 66755.67 | 0.839 ms | 0.887 ms | +0.5% |
| GET | P=1 | 256 B | 63856.96 | 67521.95 | 0.911 ms | 0.831 ms | -5.4% |
| SET | P=16 | 16 B | 800000.00 | 386100.38 | 1.703 ms | 3.079 ms | +107.2% |
| GET | P=16 | 16 B | 909090.94 | 502512.56 | 1.415 ms | 3.375 ms | +80.9% |
| SET | P=16 | 256 B | 826446.31 | 355871.91 | 1.191 ms | 4.335 ms | +132.2% |
| GET | P=16 | 256 B | 775193.81 | 480769.22 | 1.447 ms | 2.783 ms | +61.2% |
