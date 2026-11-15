#include "table.h"
#include "btree.h"
#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void serialize_row(Row* source, void* destination) {
    uint8_t* dest = (uint8_t*)destination;
    memcpy(dest + ID_OFFSET, &(source->id), ID_SIZE);
    memcpy(dest + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy(dest + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination) {
    const uint8_t* src = (const uint8_t*)source;
    memcpy(&(destination->id), src + ID_OFFSET, ID_SIZE);
    memcpy(&(destination->username), src + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email), src + EMAIL_OFFSET, EMAIL_SIZE);
}

Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);

    Table* table = malloc(sizeof(Table));
    if (table == NULL) {
        printf("Memory allocation failed for Table\n");
        exit(EXIT_FAILURE);
    }

    table->pager = pager;
    table->root_page_num = 0;
    table->wal = NULL;
    table->in_transaction = false;

    if (pager->file_length == 0) {
        void* root_node = get_page(pager, 0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
        mark_page_dirty(pager, 0);
    }

    table->wal = wal_open(filename);
    uint32_t recovered = wal_recover(table->wal, table);
    if (recovered > 0) {
        printf("[WAL] Recovered %u operation(s) after unexpected shutdown.\n", recovered);
        wal_checkpoint(table->wal, table);
    }

    return table;
}

void db_close(Table* table) {
    Pager* pager = table->pager;

    /* If exiting with an uncommitted transaction, discard uncommitted work safely */
    if (table->in_transaction) {
        printf("[TXN] Uncommitted transaction discarded on exit.\n");
        pager_discard_cache(pager);
        if (table->wal != NULL) {
            if (ftruncate(table->wal->fd, 0) == 0) {
                lseek(table->wal->fd, 0, SEEK_SET);
                fsync(table->wal->fd);
            }
            wal_close(table->wal);
            table->wal = NULL;
        }
    } else if (table->wal != NULL) {
        wal_checkpoint(table->wal, table);
        wal_close(table->wal);
        table->wal = NULL;
    }

    for (uint32_t i = 0; i < pager->max_pages; i++) {
        PageFrame* frame = pager->pages[i];
        if (frame == NULL) {
            continue;
        }
        if (frame->is_dirty) {
            pager_flush(pager, i, PAGE_SIZE);
        }
        free(frame->data);
        free(frame);
        pager->pages[i] = NULL;
    }

    int result = close(pager->file_descriptor);
    if (result == -1) {
        printf("Error closing db file: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    free(pager->pages);
    free(pager);
    free(table);
}

void print_row(Row* row) {
    printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}
