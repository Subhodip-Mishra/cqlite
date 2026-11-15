#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static void ensure_page_capacity(Pager* pager, uint32_t page_num) {
    if (page_num < pager->max_pages) {
        return;
    }
    uint32_t new_cap = pager->max_pages;
    while (new_cap <= page_num) {
        if (new_cap > UINT32_MAX / 2) {
            new_cap = page_num + 1;
            break;
        }
        new_cap *= 2;
    }
    PageFrame** new_pages = realloc(pager->pages, new_cap * sizeof(PageFrame*));
    if (new_pages == NULL) {
        printf("Memory reallocation failed for page table (%u pages)\n", new_cap);
        exit(EXIT_FAILURE);
    }
    memset(new_pages + pager->max_pages, 0, (new_cap - pager->max_pages) * sizeof(PageFrame*));
    pager->pages = new_pages;
    pager->max_pages = new_cap;
}

static void lru_detach(Pager* pager, PageFrame* frame) {
    if (frame->prev) {
        frame->prev->next = frame->next;
    } else {
        pager->lru_head = frame->next;
    }

    if (frame->next) {
        frame->next->prev = frame->prev;
    } else {
        pager->lru_tail = frame->prev;
    }

    frame->prev = NULL;
    frame->next = NULL;
}

static void lru_attach_front(Pager* pager, PageFrame* frame) {
    frame->prev = NULL;
    frame->next = pager->lru_head;

    if (pager->lru_head) {
        pager->lru_head->prev = frame;
    }
    pager->lru_head = frame;

    if (pager->lru_tail == NULL) {
        pager->lru_tail = frame;
    }
}

static void evict_lru_page(Pager* pager) {
    PageFrame* victim = pager->lru_tail;
    if (victim == NULL) {
        return;
    }

    uint32_t victim_page_num = victim->page_num;
    if (victim->is_dirty) {
        pager_flush(pager, victim_page_num, PAGE_SIZE);
    }

    lru_detach(pager, victim);
    pager->pages[victim_page_num] = NULL;
    pager->cached_pages--;
    pager->evictions++;

    free(victim->data);
    free(victim);
}

Pager* pager_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);
    if (file_length == -1) {
        printf("Error seeking file length: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    if (file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    Pager* pager = malloc(sizeof(Pager));
    if (pager == NULL) {
        printf("Memory allocation failed for Pager\n");
        exit(EXIT_FAILURE);
    }

    uint32_t num_pages = (uint32_t)(file_length / PAGE_SIZE);
    uint32_t initial_cap = INITIAL_MAX_PAGES;
    while (initial_cap <= num_pages) {
        initial_cap *= 2;
    }

    pager->pages = calloc(initial_cap, sizeof(PageFrame*));
    if (pager->pages == NULL) {
        printf("Memory allocation failed for page table\n");
        exit(EXIT_FAILURE);
    }

    pager->file_descriptor = fd;
    pager->file_length = (uint64_t)file_length;
    pager->num_pages = num_pages;
    pager->max_pages = initial_cap;

    pager->cached_pages = 0;
    pager->lru_head = NULL;
    pager->lru_tail = NULL;
    pager->cache_hits = 0;
    pager->cache_misses = 0;
    pager->evictions = 0;

    return pager;
}

void* get_page(Pager* pager, uint32_t page_num) {
    ensure_page_capacity(pager, page_num);

    /* Cache Hit: Move frame to MRU front in O(1) */
    if (pager->pages[page_num] != NULL) {
        PageFrame* frame = pager->pages[page_num];
        pager->cache_hits++;
        if (frame != pager->lru_head) {
            lru_detach(pager, frame);
            lru_attach_front(pager, frame);
        }
        return frame->data;
    }

    /* Cache Miss: Evict LRU victim if cache reached 64MB cap */
    pager->cache_misses++;
    if (pager->cached_pages >= CACHE_MAX_PAGES) {
        evict_lru_page(pager);
    }

    void* page_data = NULL;
    if (posix_memalign(&page_data, PAGE_SIZE, PAGE_SIZE) != 0 || page_data == NULL) {
        printf("Aligned memory allocation failed for page %u\n", page_num);
        exit(EXIT_FAILURE);
    }
    memset(page_data, 0, PAGE_SIZE);

    uint32_t disk_pages = (uint32_t)(pager->file_length / PAGE_SIZE);
    if (pager->file_length % PAGE_SIZE) {
        disk_pages += 1;
    }

    if (page_num < disk_pages) {
        off_t offset = (off_t)page_num * PAGE_SIZE;
        ssize_t bytes_read = pread(pager->file_descriptor, page_data, PAGE_SIZE, offset);
        if (bytes_read == -1) {
            printf("Error reading file: %d\n", errno);
            free(page_data);
            exit(EXIT_FAILURE);
        }
    }

    PageFrame* frame = malloc(sizeof(PageFrame));
    if (frame == NULL) {
        printf("Memory allocation failed for PageFrame\n");
        free(page_data);
        exit(EXIT_FAILURE);
    }

    frame->page_num = page_num;
    frame->data = page_data;
    frame->is_dirty = false;
    frame->prev = NULL;
    frame->next = NULL;

    pager->pages[page_num] = frame;
    lru_attach_front(pager, frame);
    pager->cached_pages++;

    if (page_num >= pager->num_pages) {
        pager->num_pages = page_num + 1;
    }

    return frame->data;
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size) {
    if (page_num >= pager->max_pages || pager->pages[page_num] == NULL) {
        printf("Tried to flush invalid or null page %u\n", page_num);
        exit(EXIT_FAILURE);
    }

    off_t offset = (off_t)page_num * PAGE_SIZE;
    const uint8_t* buf = (const uint8_t*)pager->pages[page_num]->data;
    size_t total_written = 0;

    while (total_written < size) {
        ssize_t bytes_written = pwrite(
            pager->file_descriptor,
            buf + total_written,
            size - total_written,
            offset + (off_t)total_written
        );

        if (bytes_written == -1) {
            if (errno == EINTR) {
                continue;
            }
            printf("Error writing: %d\n", errno);
            exit(EXIT_FAILURE);
        }

        if (bytes_written == 0) {
            printf("Error writing: unexpected zero-byte write\n");
            exit(EXIT_FAILURE);
        }

        total_written += (size_t)bytes_written;
    }

    uint64_t new_end = (uint64_t)offset + (uint64_t)total_written;
    if (new_end > pager->file_length) {
        pager->file_length = new_end;
    }
}

uint32_t get_unused_page_num(Pager* pager) {
    return pager->num_pages;
}

void mark_page_dirty(Pager* pager, uint32_t page_num) {
    if (page_num < pager->max_pages && pager->pages[page_num] != NULL) {
        pager->pages[page_num]->is_dirty = true;
    }
}
void pager_discard_cache(Pager* pager) {
    for (uint32_t i = 0; i < pager->max_pages; i++) {
        PageFrame* frame = pager->pages[i];
        if (frame != NULL) {
            free(frame->data);
            free(frame);
            pager->pages[i] = NULL;
        }
    }
    pager->cached_pages = 0;
    pager->lru_head = NULL;
    pager->lru_tail = NULL;
    pager->num_pages = (uint32_t)(pager->file_length / PAGE_SIZE);
}
