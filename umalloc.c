#include "umalloc.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const char allocator_author[] = "Alec Hrdina";

static mem_block_header_t *free_heads[BIN_COUNT];
static size_t total_heap_payload_bytes;

static const uintptr_t kSentinelTag = 0xFEEDFACEu;
static const uintptr_t kAllocCanary = 0xDEADBEEFu;

static int select_bin_index(size_t payload_size)
{
    if (payload_size <= 512) {
        return 0;
    }
    if (payload_size <= 2048) {
        return 1;
    }
    if (payload_size <= 8192) {
        return 2;
    }
    if (payload_size <= 16384) {
        return 3;
    }
    if (payload_size <= 32768) {
        return 4;
    }
    return 5;
}

static bool is_allocated(const mem_block_header_t *block)
{
    assert(block != NULL);
    return (block->block_metadata & (size_t)0x1) != 0;
}

static void mark_allocated(mem_block_header_t *block)
{
    assert(block != NULL);
    block->block_metadata |= (size_t)0x1;
    block->next = (mem_block_header_t *)kAllocCanary;
}

static void mark_deallocated(mem_block_header_t *block)
{
    assert(block != NULL);
    block->block_metadata &= ~((size_t)0x1);
}

static void set_bin_index(mem_block_header_t *block, int bin_idx)
{
    assert(block != NULL);
    assert(bin_idx >= 0 && bin_idx < BIN_COUNT);
    block->block_metadata =
        (block->block_metadata & ~((size_t)0xE)) | (((size_t)bin_idx & 0x7) << 1);
}

static int get_bin_index(const mem_block_header_t *block)
{
    assert(block != NULL);
    return (int)((block->block_metadata >> 1) & 0x7);
}

static size_t get_size(const mem_block_header_t *block)
{
    assert(block != NULL);
    return block->block_metadata & ~((size_t)(ALIGNMENT - 1));
}

static bool is_sentinel(const mem_block_header_t *block)
{
    return block != NULL && block->next == (mem_block_header_t *)kSentinelTag;
}

static void set_block_metadata(
    mem_block_header_t *block,
    size_t size,
    int bin_idx,
    bool allocated,
    mem_block_header_t *next)
{
    assert(block != NULL);
    block->block_metadata = ALIGN(size);
    set_bin_index(block, bin_idx);
    if (allocated) {
        mark_allocated(block);
    } else {
        mark_deallocated(block);
    }
    block->next = next;
}

static void *get_payload(mem_block_header_t *block)
{
    assert(block != NULL);
    return (void *)(block + 1);
}

static mem_block_header_t *get_header(void *payload)
{
    assert(payload != NULL);
    return ((mem_block_header_t *)payload) - 1;
}

static mem_block_header_t *get_next_adjacent_block(mem_block_header_t *block)
{
    assert(block != NULL);
    return (mem_block_header_t *)((char *)block + sizeof(*block) + get_size(block));
}

static void insert_sorted_free_block(mem_block_header_t *block, int bin_idx)
{
    mem_block_header_t *curr = free_heads[bin_idx];
    mem_block_header_t *prev = NULL;

    while (curr != NULL && curr < block) {
        prev = curr;
        curr = curr->next;
    }

    block->next = curr;
    if (prev == NULL) {
        free_heads[bin_idx] = block;
    } else {
        prev->next = block;
    }
}

static mem_block_header_t *coalesce(
    mem_block_header_t *curr,
    mem_block_header_t *prev,
    int bin_idx)
{
    mem_block_header_t *result = curr;
    if (curr == NULL) {
        return NULL;
    }

    if (curr->next != NULL) {
        mem_block_header_t *adjacent_next = get_next_adjacent_block(curr);
        if (curr->next == adjacent_next) {
            size_t merged_size = get_size(curr) + get_size(curr->next) + sizeof(*curr);
            mem_block_header_t *next_next = curr->next->next;
            set_block_metadata(curr, merged_size, bin_idx, false, next_next);
        }
    }

    if (prev != NULL) {
        mem_block_header_t *adjacent_from_prev = get_next_adjacent_block(prev);
        if (adjacent_from_prev == curr) {
            size_t merged_size = get_size(prev) + get_size(curr) + sizeof(*curr);
            set_block_metadata(prev, merged_size, bin_idx, false, curr->next);
            result = prev;
        }
    }

    return result;
}

static mem_block_header_t *find_first_fit(
    int bin_idx,
    size_t payload_size,
    mem_block_header_t **prev_out)
{
    mem_block_header_t *prev = NULL;
    mem_block_header_t *curr = free_heads[bin_idx];

    while (curr != NULL) {
        if (get_size(curr) >= payload_size) {
            *prev_out = prev;
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }

    *prev_out = NULL;
    return NULL;
}

static void *grow_heap_region(size_t bytes)
{
    void *region = malloc(bytes);
    if (region == NULL) {
        return NULL;
    }
    assert((((uintptr_t)region) & (ALIGNMENT - 1)) == 0);
    return region;
}

static mem_block_header_t *extend(size_t payload_size)
{
    size_t minimum_needed = payload_size + sizeof(mem_block_header_t);
    size_t chunk_size = PAGE_SIZE / 4;
    int bin_idx = select_bin_index(payload_size);

    while (minimum_needed > chunk_size) {
        chunk_size *= 2;
    }

    size_t request_size = chunk_size + (3 * sizeof(mem_block_header_t));
    mem_block_header_t *heap_start = (mem_block_header_t *)grow_heap_region(request_size);
    if (heap_start == NULL) {
        return NULL;
    }

    size_t free_payload_size = request_size - (2 * sizeof(mem_block_header_t));
    set_block_metadata(heap_start, free_payload_size, bin_idx, false, NULL);
    total_heap_payload_bytes += free_payload_size;

    mem_block_header_t *sentinel =
        (mem_block_header_t *)((char *)heap_start + request_size - sizeof(mem_block_header_t));
    set_block_metadata(sentinel, 0, bin_idx, true, (mem_block_header_t *)kSentinelTag);

    insert_sorted_free_block(heap_start, bin_idx);
    return heap_start;
}

static mem_block_header_t *split(mem_block_header_t *block, size_t payload_size)
{
    size_t block_size = get_size(block) + sizeof(mem_block_header_t);
    size_t alloc_size = payload_size + sizeof(mem_block_header_t);
    int bin_idx = get_bin_index(block);

    if (block_size <= alloc_size) {
        return block;
    }

    size_t remain = block_size - alloc_size;
    bool should_split = false;

    mem_block_header_t *adjacent = get_next_adjacent_block(block);
    if (!is_sentinel(adjacent) && !is_allocated(adjacent) && remain > 0) {
        should_split = true;
    } else if (remain >= sizeof(mem_block_header_t) + ALIGNMENT &&
               remain >= (block_size / 4)) {
        should_split = true;
    }

    if (!should_split) {
        block->block_metadata = get_size(block);
        return block;
    }

    block->block_metadata = ALIGN(payload_size);

    mem_block_header_t *remainder = (mem_block_header_t *)((char *)block + alloc_size);
    set_block_metadata(
        remainder,
        remain - sizeof(mem_block_header_t),
        bin_idx,
        true,
        NULL);
    ufree(get_payload(remainder));

    return block;
}

int uinit(void)
{
    for (int i = 0; i < BIN_COUNT; i++) {
        free_heads[i] = NULL;
    }
    total_heap_payload_bytes = 0;
    return 0;
}

size_t umalloc_heap_payload_bytes(void)
{
    return total_heap_payload_bytes;
}

void *umalloc(size_t payload_size)
{
    if (payload_size == 0) {
        return NULL;
    }

    payload_size = ALIGN(payload_size);
    int home_bin = select_bin_index(payload_size);
    int selected_bin = home_bin;

    mem_block_header_t *selected_block = NULL;
    mem_block_header_t *selected_prev = NULL;

    selected_block = find_first_fit(home_bin, payload_size, &selected_prev);
    if (selected_block == NULL) {
        for (int i = home_bin + 1; i < BIN_COUNT; i++) {
            selected_block = find_first_fit(i, payload_size, &selected_prev);
            if (selected_block != NULL) {
                selected_bin = i;
                break;
            }
        }
    }

    if (selected_block == NULL) {
        if (extend(payload_size) == NULL) {
            return NULL;
        }
        selected_bin = home_bin;
        selected_block = find_first_fit(home_bin, payload_size, &selected_prev);
    }

    if (selected_block == NULL) {
        return NULL;
    }

    if (selected_prev == NULL) {
        free_heads[selected_bin] = selected_block->next;
    } else {
        selected_prev->next = selected_block->next;
    }

    selected_block = split(selected_block, payload_size);
    set_block_metadata(
        selected_block,
        get_size(selected_block),
        selected_bin,
        true,
        NULL);

    return get_payload(selected_block);
}

void ufree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    mem_block_header_t *block = get_header(ptr);
    if (!is_allocated(block)) {
        fprintf(stderr, "ufree: attempted double-free\n");
        return;
    }

    int bin_idx = get_bin_index(block);
    mark_deallocated(block);

    mem_block_header_t *curr = free_heads[bin_idx];
    mem_block_header_t *prev = NULL;

    while (curr != NULL && curr < block) {
        prev = curr;
        curr = curr->next;
    }

    block->next = curr;
    if (prev == NULL) {
        free_heads[bin_idx] = block;
        free_heads[bin_idx] = coalesce(block, NULL, bin_idx);
    } else {
        prev->next = block;
        (void)coalesce(block, prev, bin_idx);
    }
}
