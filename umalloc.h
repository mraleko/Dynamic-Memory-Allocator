#ifndef UMALLOC_H
#define UMALLOC_H

#include <stdbool.h>
#include <stddef.h>

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define BIN_COUNT 6
#define PAGE_SIZE 4096

typedef struct mem_block_header_struct {
    size_t block_metadata;
    struct mem_block_header_struct *next;
} mem_block_header_t;

int uinit(void);
void *umalloc(size_t payload_size);
void ufree(void *ptr);
size_t umalloc_heap_payload_bytes(void);

#endif
