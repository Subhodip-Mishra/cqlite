#ifndef WAL_H
#define WAL_H

#include <stdint.h>
#include <stdbool.h>

#define WAL_MAGIC 0x4351574CUL /* "CQWL" */

typedef enum {
    WAL_OP_INSERT = 1,
    WAL_OP_UPDATE = 2,
    WAL_OP_DELETE = 3
} WalOpType;


typedef struct Wal Wal;

typedef struct Wal {
    int fd;
    char* filename;
    uint64_t record_count;
} Wal;

struct Table;
struct Row;

Wal* wal_open(const char* db_filename);
void wal_append(Wal* wal, WalOpType op, uint32_t key, const struct Row* row, bool sync_disk);
uint32_t wal_recover(Wal* wal, struct Table* table);
void wal_checkpoint(Wal* wal, struct Table* table);
void wal_close(Wal* wal);

#endif // WAL_H
