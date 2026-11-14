#ifndef PAGER_H
#define PAGER_H

#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define INITIAL_MAX_PAGES 1024
#define CACHE_MAX_PAGES 16384  /* 16384 * 4KB = 64 MB Fixed RAM Cap */

typedef struct PageFrame {
    uint32_t page_num;
    void* data;
    bool is_dirty;
    struct PageFrame* prev;
    struct PageFrame* next;
} PageFrame;

typedef struct {
    int file_descriptor;
    uint64_t file_length;
    uint32_t num_pages;
    uint32_t max_pages;
    PageFrame** pages;

    /* LRU Cache Tracking */
    uint32_t cached_pages;
    PageFrame* lru_head;  /* Most Recently Used */
    PageFrame* lru_tail;  /* Least Recently Used */
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t evictions;
} Pager;

Pager* pager_open(const char* filename);
void* get_page(Pager* pager, uint32_t page_num);
void pager_flush(Pager* pager, uint32_t page_num, uint32_t size);
uint32_t get_unused_page_num(Pager* pager);
void mark_page_dirty(Pager* pager, uint32_t page_num);

#endif // PAGER_H