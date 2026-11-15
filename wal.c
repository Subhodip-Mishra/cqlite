#define _POSIX_C_SOURCE 200809L
#include "wal.h"
#include "btree.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t op;
    uint32_t key;
    Row row;
    uint32_t crc32;
} WalRecord;
#pragma pack(pop)

static uint32_t compute_crc32(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

Wal* wal_open(const char* db_filename) {
    size_t len = strlen(db_filename) + 5; /* ".wal\0" */
    char* wal_filename = malloc(len);
    if (wal_filename == NULL) {
        printf("Memory allocation failed for WAL filename\n");
        exit(EXIT_FAILURE);
    }
    snprintf(wal_filename, len, "%s.wal", db_filename);

    int fd = open(wal_filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open WAL file '%s': %d\n", wal_filename, errno);
        free(wal_filename);
        exit(EXIT_FAILURE);
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        printf("Error seeking WAL file: %d\n", errno);
        close(fd);
        free(wal_filename);
        exit(EXIT_FAILURE);
    }

    Wal* wal = malloc(sizeof(Wal));
    if (wal == NULL) {
        printf("Memory allocation failed for Wal struct\n");
        close(fd);
        free(wal_filename);
        exit(EXIT_FAILURE);
    }

    wal->fd = fd;
    wal->filename = wal_filename;
    wal->record_count = (uint64_t)size / sizeof(WalRecord);
    return wal;
}

void wal_append(Wal* wal, WalOpType op, uint32_t key, const Row* row, bool sync_disk) {
    if (wal == NULL) {
        return;
    }

    WalRecord rec;
    memset(&rec, 0, sizeof(WalRecord));
    rec.magic = WAL_MAGIC;
    rec.op = (uint8_t)op;
    rec.key = key;
    if (row != NULL) {
        memcpy(&rec.row, row, sizeof(Row));
    }
    rec.crc32 = compute_crc32(&rec, sizeof(WalRecord) - sizeof(uint32_t));

    off_t offset = lseek(wal->fd, 0, SEEK_END);
    if (offset == -1) {
        printf("Error seeking end of WAL: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    const uint8_t* buf = (const uint8_t*)&rec;
    size_t total_written = 0;
    while (total_written < sizeof(WalRecord)) {
        ssize_t written = pwrite(
            wal->fd,
            buf + total_written,
            sizeof(WalRecord) - total_written,
            offset + (off_t)total_written
        );
        if (written == -1) {
            if (errno == EINTR) continue;
            printf("Error writing WAL record: %d\n", errno);
            exit(EXIT_FAILURE);
        }
        total_written += (size_t)written;
    }

    if (sync_disk) {
        fsync(wal->fd);
    }
    wal->record_count++;
}

uint32_t wal_recover(Wal* wal, Table* table) {
    if (wal == NULL) {
        return 0;
    }

    off_t file_size = lseek(wal->fd, 0, SEEK_END);
    if (file_size <= 0) {
        return 0;
    }

    uint32_t recovered = 0;
    off_t offset = 0;

    while (offset + (off_t)sizeof(WalRecord) <= file_size) {
        WalRecord rec;
        ssize_t bytes_read = pread(wal->fd, &rec, sizeof(WalRecord), offset);
        if (bytes_read != (ssize_t)sizeof(WalRecord)) {
            break;
        }

        uint32_t expected_crc = compute_crc32(&rec, sizeof(WalRecord) - sizeof(uint32_t));
        if (rec.magic != WAL_MAGIC || rec.crc32 != expected_crc) {
            /* Stop replay cleanly if a torn/corrupt tail frame is encountered */
            printf("[WAL] Corrupt or torn frame at offset %ld; halting replay.\n", (long)offset);
            break;
        }

        Cursor* cursor = table_find(table, rec.key);
        void* node = get_page(table->pager, cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        bool exists = (cursor->cell_num < num_cells && *leaf_node_key(node, cursor->cell_num) == rec.key);

        if (rec.op == WAL_OP_INSERT) {
            if (exists) {
                leaf_node_update(cursor, rec.key, &rec.row);
            } else {
                leaf_node_insert(cursor, rec.key, &rec.row);
            }
            recovered++;
        } else if (rec.op == WAL_OP_UPDATE) {
            if (exists) {
                leaf_node_update(cursor, rec.key, &rec.row);
                recovered++;
            }
        } else if (rec.op == WAL_OP_DELETE) {
            if (exists) {
                leaf_node_delete(cursor, rec.key);
                recovered++;
            }
        }

        free(cursor);
        offset += (off_t)sizeof(WalRecord);
    }

    return recovered;
}

void wal_checkpoint(Wal* wal, Table* table) {
    if (wal == NULL || table == NULL) {
        return;
    }

    Pager* pager = table->pager;
    for (uint32_t i = 0; i < pager->max_pages; i++) {
        if (pager->pages[i] != NULL && pager->pages[i]->is_dirty) {
            pager_flush(pager, i, PAGE_SIZE);
            pager->pages[i]->is_dirty = false;
        }
    }
    fsync(pager->file_descriptor);

    if (ftruncate(wal->fd, 0) == 0) {
        lseek(wal->fd, 0, SEEK_SET);
        fsync(wal->fd);
        wal->record_count = 0;
    }
}

void wal_close(Wal* wal) {
    if (wal == NULL) {
        return;
    }
    close(wal->fd);
    free(wal->filename);
    free(wal);
}