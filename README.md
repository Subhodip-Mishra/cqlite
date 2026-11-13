# cqlite

An embedded relational database engine built from scratch in pure C, inspired by SQLite.

## Architecture
`SQL String` ➔ `Tokenizer / Parser` ➔ `AST` ➔ `Bytecode` ➔ `Virtual Machine` ➔ `B+ Tree & 4KB Pager` ➔ `Disk (.db)`

## Current Features
* **REPL & Statement Compiler:** Custom input buffer, meta-command execution (`.exit`), and deterministic parsing for `insert` and `select`.
* **Binary Row Serialization:** Fixed-size (`291-byte`) binary serialization packing records (`id`, `username`, `email`) directly onto raw memory offsets.
* **Hardened 4KB Pager:**
  * `4096-byte` hardware-aligned pages via `posix_memalign()`.
  * $O(1)$ in-memory page cache (`void* pages[100]`) with lazy disk reads and zero-initialization via `memset()`.
  * Atomic disk persistence using `pread()` and `pwrite()` syscalls with dynamic file length tracking.

## Build & Run

### Using Make
```bash
make
./cqlite test.db