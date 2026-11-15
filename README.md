# cqlite

An embedded relational database engine built from scratch in pure C11 (`-Wall -Wextra -Werror`), inspired by SQLite.

## Architecture
`SQL String` ➔ `Tokenizer / Parser` ➔ `Statement Compiler` ➔ `Write-Ahead Log (WAL + CRC32)` ➔ `B+ Tree Engine` ➔ `64MB LRU Buffer Pool & 4KB Pager` ➔ `Disk (.db / .wal)`

## Current Features
- **Full CRUD & REPL Engine:** Deterministic parsing and execution for `insert`, `select` (full scan & `where id = <key>` point lookups), `update`, and `delete`, alongside meta-commands (`.exit`, `.btree`, `.stats`, `.checkpoint`).
- **Write-Ahead Log (WAL) & Crash Recovery:**
  - Append-only binary WAL framing (`Magic` + `OpType` + `Key` + `Row Payload` + `IEEE 802.3 CRC32` checksum) written prior to B+ Tree page mutations.
  - Automatic idempotent crash recovery on startup (`db_open`) with torn-frame detection and `.checkpoint` log truncation via `ftruncate()` + `fsync()`.
- **ACID Transactions & Group Commit:**
  - Explicit `begin`, `commit`, and `rollback` transaction control.
  - **Group Commit:** Defers per-statement `fsync()` calls inside active transactions until `commit`, flushing buffered WAL frames in a single atomic disk sync.
  - **Atomic Rollback:** Discards dirty in-memory cache frames and truncates uncommitted WAL records cleanly.
- **Binary Row Serialization:** Fixed-size (`293-byte`) binary serialization packing records (`id`, `username`, `email`) directly onto raw memory offsets.
- **Disk-Backed B+ Tree Storage Engine:**
  - Multi-level Internal Nodes (key-child routing) and Leaf Nodes (sorted 293-byte data cells) packed inside 4KB pages.
  - Binary search within nodes for $O(\log N)$ point lookups, in-place updates, cell deletion, and cursor-based sequential scans.
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

## Usage Examples

### CRUD & Point Lookups
```sql
cqlite > insert 1 suvodeep suvodeep@valko.studio
cqlite > select where id = 1
(1, suvodeep, suvodeep@valko.studio)
cqlite > update 1 suvodeep founder@valko.studio
cqlite > delete where id = 1