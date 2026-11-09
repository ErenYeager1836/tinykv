# tinykv vs Valkey Benchmark Results

## Environment & System Info
- **CPU Model**: `11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz`
- **Linux Kernel**: `7.2.11-arch1-1`
- **Compiler**: `g++ (GCC) 16.2.1 20260810`
- **tinykv Version**: `v0.1.0 (Release)`
- **Comparison Server**: `Valkey server v=9.1.2 sha=7f1dffed:1 malloc=jemalloc-5.3.1 bits=64 build=ab3e536b46916906`
- **Benchmark Tool**: `valkey-benchmark 9.1.2 (git:7f1dffed-dirty)`
- **Parameters**: `-n 100000`, `-c 50`, 5 runs per configuration (reporting median)

## Performance Comparison Table

| Workload | Pipeline | Value Size | tinykv (req/sec) | Valkey (req/sec) | tinykv p99 (ms) | Valkey p99 (ms) | Speedup (%) |
|:---------|:--------:|:----------:|:----------------:|:-----------------------------:|:---------------:|:----------------------------:|:-----------:|
| SET | P=1 | 16 B | 74850.30 | 76745.97 | 0.711 ms | 0.575 ms | -2.5% |
| GET | P=1 | 16 B | 74404.77 | 77041.60 | 0.751 ms | 0.695 ms | -3.4% |
| SET | P=1 | 256 B | 74515.65 | 78125.00 | 0.719 ms | 0.743 ms | -4.6% |
| GET | P=1 | 256 B | 73046.02 | 77041.60 | 0.687 ms | 0.743 ms | -5.2% |
| SET | P=16 | 16 B | 970873.81 | 460829.50 | 0.615 ms | 2.175 ms | +110.7% |
| GET | P=16 | 16 B | 1020408.19 | 581395.31 | 0.639 ms | 1.807 ms | +75.5% |
| SET | P=16 | 256 B | 952381.00 | 460829.50 | 0.647 ms | 2.135 ms | +106.7% |
| GET | P=16 | 256 B | 934579.44 | 529100.56 | 0.671 ms | 2.231 ms | +76.6% |
