#define _POSIX_C_SOURCE 200809L
#include "pager.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

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

    Pager* pager = malloc(sizeof(Pager));
    if (pager == NULL) {
        printf("Memory allocation failed for Pager\n");
        exit(EXIT_FAILURE);
    }

    pager->file_descriptor = fd;
    pager->file_length = (uint32_t)file_length;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %d > %d\n", page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    // Cache Miss: Allocate 4KB page-aligned memory & zero-initialize
    if (pager->pages[page_num] == NULL) {
        void* page = NULL;
        if (posix_memalign(&page, PAGE_SIZE, PAGE_SIZE) != 0 || page == NULL) {
            printf("Aligned memory allocation failed for page %d\n", page_num);
            exit(EXIT_FAILURE);
        }
        memset(page, 0, PAGE_SIZE);

        uint32_t num_pages = pager->file_length / PAGE_SIZE;
        if (pager->file_length % PAGE_SIZE) {
            num_pages += 1;
        }

        if (page_num < num_pages) {
            off_t offset = (off_t)page_num * PAGE_SIZE;
            ssize_t bytes_read = pread(pager->file_descriptor, page, PAGE_SIZE, offset);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                free(page);
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;
    }

    // Cache Hit: Return O(1) aligned pointer
    return pager->pages[page_num];
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to flush page number out of bounds. %d >= %d\n", page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }
    
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = (off_t)page_num * PAGE_SIZE;
    const uint8_t* buf = (const uint8_t*)pager->pages[page_num];
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

    // Update file_length when file extends
    uint32_t new_end = (uint32_t)offset + (uint32_t)total_written;
    if (new_end > pager->file_length) {
        pager->file_length = new_end;
    }
}