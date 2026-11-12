An embedded relational database engine built from scratch in pure C, inspired by SQLite.

## Architecture
`SQL String` ➔ `Tokenizer / Parser` ➔ `AST` ➔ `Bytecode` ➔ `Virtual Machine` ➔ `B+ Tree & 4KB Pager` ➔ `Disk (.db)`

## Build & Run
```bash
gcc -Wall -Wextra -std=c11 -g main.c -o cqlite
./cqlite