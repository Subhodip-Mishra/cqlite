# cqlite

An embedded relational database engine built from scratch in pure C11 (`-Wall -Wextra -Werror`), inspired by SQLite.

## Architecture
`SQL String` ➔ `Tokenizer / Parser` ➔ `AST` ➔ `Bytecode` ➔ `Virtual Machine` ➔ `B+ Tree Engine` ➔ `64MB LRU Buffer Pool & 4KB Pager` ➔ `Disk (.db)`

## Current Features
- **REPL & Statement Compiler:** Custom input buffer, meta-command execution (`.exit`, `.btree`, `.constants`), and deterministic parsing for `insert` and `select`.
- **Binary Row Serialization:** Fixed-size (`291-byte`) binary serialization packing records (`id`, `username`, `email`) directly onto raw memory offsets.
- **Disk-Backed B+ Tree Storage Engine:**
  - Multi-level Internal Nodes (key-child routing) and Leaf Nodes (sorted 291-byte data cells) packed inside 4KB slotted pages.
  - Binary search within nodes for $O(\log N)$ point lookups and cursor-based sequential scans.
  - Recursive leaf and internal node splitting for dynamic tree growth.
- **Hardened 4KB Pager & 64 MB LRU Buffer Pool:**
  - `4096-byte` hardware-aligned pages via `posix_memalign()`.
  - Fixed-cap **LRU Page Cache** (`16,384` frames = `64 MB` max RAM) using an $O(1)$ doubly-linked list and direct page lookup table.
  - **Dirty-Bit Optimization (`is_dirty`):** Tracks modified pages in memory so read-heavy workloads (`select`) trigger zero redundant `pwrite()` syscalls during cache eviction.
  - **64-Bit File Offset Addressing:** Full `uint64_t` file length and offset tracking supporting `>11 GB` database files without 32-bit integer overflow.
  - Atomic disk persistence using `pread()` and `pwrite()` syscalls with lazy disk reads and zero-initialization via `memset()`.

## Benchmarks (1M to 20M Rows)

Tested on Linux (Ubuntu 24.04 LTS, SSD, `gcc -O3`) under a strict **64 MB fixed RAM buffer pool** (`16,384` cached pages) with `10,000` verified random $O(\log N)$ point lookups per run:

| Dataset Size | Disk Footprint | RAM Cap | Insert Throughput | Avg Lookup Latency | Query Throughput | Cache Hit Rate |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1,000,000 rows** | 560.23 MB | 64.00 MB | **1,556,366 rows/sec** (0.64s) | **4.75 µs / query** | **210.58 K QPS** | 90.15% |
| **10,000,000 rows** | 5.60 GB | 64.00 MB | **868,980 rows/sec** (11.51s) | **229.25 µs / query** | **4.36 K QPS** | 83.82% |
| **20,000,000 rows** | 11.20 GB | 64.00 MB | **394,679 rows/sec** (50.67s) | **248.92 µs / query** | **4.02 K QPS** | 81.57% |

> **Key Takeaway:** Scaling from **5.60 GB (10M rows)** to **11.20 GB (20M rows)** increases average random lookup latency by less than **9%** (`229.25 µs` → `248.92 µs`), confirming logarithmic $O(\log N)$ B+ Tree scaling within a bounded 64 MB memory budget.

## Build & Run

### Using Make
```bash
make
./cqlite test.db