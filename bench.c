#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "table.h"
#include "btree.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void run_benchmark(const char* db_file, uint32_t num_rows, uint32_t num_lookups) {
    unlink(db_file);
    Table* table = db_open(db_file);

    Row row;
    double t0 = now_sec();

    for (uint32_t i = 1; i <= num_rows; i++) {
        row.id = i;
        snprintf(row.username, sizeof(row.username), "user_%u", i);
        snprintf(row.email, sizeof(row.email), "user_%u@valko.studio", i);

        Cursor* cursor = table_find(table, i);
        leaf_node_insert(cursor, i, &row);
        free(cursor);

        if (i % 1000000 == 0) {
            printf("\r  [Inserting %uM / %uM rows... %.1fs]", i / 1000000, num_rows / 1000000, now_sec() - t0);
            fflush(stdout);
        }
    }
    printf("\r                                                \r");

    double t1 = now_sec();

    /* Reset cache stats before measuring pure lookup performance */
    table->pager->cache_hits = 0;
    table->pager->cache_misses = 0;
    table->pager->evictions = 0;

    /* Random O(log N) Point Lookups */
    srand(42);
    uint64_t found_count = 0;
    double t2 = now_sec();

    for (uint32_t q = 0; q < num_lookups; q++) {
        uint32_t target = ((uint32_t)rand() % num_rows) + 1;
        Cursor* cursor = table_find(table, target);
        void* node = get_page(table->pager, cursor->page_num);
        if (*leaf_node_key(node, cursor->cell_num) == target) {
            found_count++;
        }
        free(cursor);
    }

    double t3 = now_sec();

    double insert_time = t1 - t0;
    double lookup_time = t3 - t2;
    double avg_lookup_us = (lookup_time / (double)num_lookups) * 1e6;
    double db_size_mb = ((double)table->pager->num_pages * PAGE_SIZE) / (1024.0 * 1024.0);
    double ram_used_mb = ((double)table->pager->cached_pages * PAGE_SIZE) / (1024.0 * 1024.0);

    printf("=== Benchmark: %u Rows (Verified: %lu/%u) ===\n", num_rows, found_count, num_lookups);
    printf("  Insert Time:      %.3f s (%.0f rows/sec)\n", insert_time, num_rows / insert_time);
    printf("  Disk Pages:       %u (%.2f MB on disk) | RAM Cached: %.2f MB\n",
           table->pager->num_pages, db_size_mb, ram_used_mb);
    printf("  Lookup Cache:     Hits=%lu | Misses=%lu | Evictions=%lu\n",
           table->pager->cache_hits, table->pager->cache_misses, table->pager->evictions);
    printf("  %u Random Lookups: %.4f s | Avg Latency: %.2f us/query (%.2f K queries/sec)\n\n",
           num_lookups, lookup_time, avg_lookup_us, (num_lookups / lookup_time) / 1e3);
    fflush(stdout);

    db_close(table);
    unlink(db_file);
}

int main(void) {
    printf("Running cqlite LRU-Cached B-Tree Benchmark (64 MB Fixed Buffer Pool)...\n\n");
    fflush(stdout);
    run_benchmark("bench_1m.db",  1000000,  10000);
    run_benchmark("bench_10m.db", 10000000, 10000);
    run_benchmark("bench_20m.db", 20000000, 10000);
    return 0;
}