#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef USE_LIBC
#include "umalloc.h"
#endif

typedef struct trace_stats_struct {
    size_t ops;
    size_t allocs;
    size_t frees;
} trace_stats_t;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return path;
    }
    return slash + 1;
}

#ifndef USE_LIBC
static const char *kModeName = "umalloc";

static int allocator_init(void)
{
    return uinit();
}

static void *allocator_alloc(size_t size)
{
    return umalloc(size);
}

static void allocator_free(void *ptr)
{
    ufree(ptr);
}

static void *allocator_realloc(void *old_ptr, size_t old_size, size_t new_size)
{
    if (old_ptr == NULL) {
        return allocator_alloc(new_size);
    }
    if (new_size == 0) {
        allocator_free(old_ptr);
        return NULL;
    }

    void *new_ptr = allocator_alloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_size = old_size < new_size ? old_size : new_size;
    if (copy_size > 0) {
        memcpy(new_ptr, old_ptr, copy_size);
    }
    allocator_free(old_ptr);
    return new_ptr;
}

static size_t allocator_heap_size_bytes(void)
{
    return umalloc_heap_payload_bytes();
}
#else
static const char *kModeName = "libc_malloc";

static int allocator_init(void)
{
    return 0;
}

static void *allocator_alloc(size_t size)
{
    return malloc(size);
}

static void allocator_free(void *ptr)
{
    free(ptr);
}

static void *allocator_realloc(void *old_ptr, size_t old_size, size_t new_size)
{
    (void)old_size;
    return realloc(old_ptr, new_size);
}

static size_t allocator_heap_size_bytes(void)
{
    return 0;
}
#endif

static int run_trace(
    const char *trace_path,
    trace_stats_t *stats_out,
    double *elapsed_ms_out,
    double *avg_utilization_out)
{
    FILE *trace = fopen(trace_path, "r");
    if (trace == NULL) {
        fprintf(stderr, "failed to open trace: %s\n", trace_path);
        return 1;
    }

    size_t num_ids = 0;
    size_t expected_ops = 0;
    if (fscanf(trace, "%zu", &num_ids) != 1 || fscanf(trace, "%zu", &expected_ops) != 1) {
        fprintf(stderr, "invalid trace header: %s\n", trace_path);
        fclose(trace);
        return 1;
    }

    void **slots = (void **)calloc(num_ids, sizeof(void *));
    size_t *slot_sizes = (size_t *)calloc(num_ids, sizeof(size_t));
    bool *slot_active = (bool *)calloc(num_ids, sizeof(bool));
    if (slots == NULL || slot_sizes == NULL || slot_active == NULL) {
        fprintf(stderr, "failed to allocate trace state arrays\n");
        free(slots);
        free(slot_sizes);
        free(slot_active);
        fclose(trace);
        return 1;
    }

    trace_stats_t stats = {0, 0, 0};
    size_t total_allocated = 0;
    double utilization_accumulator = 0.0;
    double start = now_ms();

    char op = '\0';
    while (fscanf(trace, " %c", &op) == 1) {
        size_t id = 0;
        size_t size = 0;

        if (op == 'a') {
            if (fscanf(trace, "%zu %zu", &id, &size) != 2) {
                fprintf(stderr, "invalid alloc line in %s\n", trace_path);
                goto fail;
            }
            if (id >= num_ids) {
                fprintf(stderr, "alloc id out of bounds in %s\n", trace_path);
                goto fail;
            }
            if (slot_active[id] && slots[id] != NULL) {
                allocator_free(slots[id]);
                slots[id] = NULL;
            }
            slots[id] = allocator_alloc(size);
            if (size > 0 && slots[id] == NULL) {
                fprintf(stderr, "allocation failed for id %zu in %s\n", id, trace_path);
                goto fail;
            }
            if (slot_active[id]) {
                total_allocated -= slot_sizes[id];
            }
            slot_sizes[id] = size;
            slot_active[id] = true;
            total_allocated += size;
            stats.allocs++;
            stats.ops++;
            size_t heap_size = allocator_heap_size_bytes();
#ifndef USE_LIBC
            utilization_accumulator +=
                heap_size > 0 ? ((double)total_allocated / (double)heap_size) : 0.0;
#else
            (void)heap_size;
#endif
            continue;
        }

        if (op == 'f') {
            if (fscanf(trace, "%zu", &id) != 1) {
                fprintf(stderr, "invalid free line in %s\n", trace_path);
                goto fail;
            }
            if (id >= num_ids) {
                fprintf(stderr, "free id out of bounds in %s\n", trace_path);
                goto fail;
            }
            if (slot_active[id]) {
                total_allocated -= slot_sizes[id];
                slot_active[id] = false;
            }
            if (slots[id] != NULL) {
                allocator_free(slots[id]);
                slots[id] = NULL;
            }
            stats.frees++;
            stats.ops++;
            size_t heap_size = allocator_heap_size_bytes();
#ifndef USE_LIBC
            utilization_accumulator +=
                heap_size > 0 ? ((double)total_allocated / (double)heap_size) : 0.0;
#else
            (void)heap_size;
#endif
            continue;
        }

        if (op == 'r') {
            if (fscanf(trace, "%zu %zu", &id, &size) != 2) {
                fprintf(stderr, "invalid realloc line in %s\n", trace_path);
                goto fail;
            }
            if (id >= num_ids) {
                fprintf(stderr, "realloc id out of bounds in %s\n", trace_path);
                goto fail;
            }
            void *next = allocator_realloc(slots[id], slot_sizes[id], size);
            if (size > 0 && next == NULL) {
                fprintf(stderr, "realloc failed for id %zu in %s\n", id, trace_path);
                goto fail;
            }
            if (size == 0) {
                if (slot_active[id]) {
                    total_allocated -= slot_sizes[id];
                    slot_active[id] = false;
                }
                slots[id] = NULL;
            } else {
                if (slot_active[id]) {
                    total_allocated -= slot_sizes[id];
                }
                slots[id] = next;
                slot_sizes[id] = size;
                slot_active[id] = true;
                total_allocated += size;
            }
            stats.ops++;
            size_t heap_size = allocator_heap_size_bytes();
#ifndef USE_LIBC
            utilization_accumulator +=
                heap_size > 0 ? ((double)total_allocated / (double)heap_size) : 0.0;
#else
            (void)heap_size;
#endif
            continue;
        }

        fprintf(stderr, "unknown op '%c' in %s\n", op, trace_path);
        goto fail;
    }

    *elapsed_ms_out = now_ms() - start;
    *stats_out = stats;
#ifndef USE_LIBC
    *avg_utilization_out =
        stats.ops > 0 ? (utilization_accumulator / (double)stats.ops) : 0.0;
#else
    (void)total_allocated;
    (void)utilization_accumulator;
    *avg_utilization_out = NAN;
#endif

    if (stats.ops != expected_ops) {
        fprintf(
            stderr,
            "warning: expected %zu ops but processed %zu in %s\n",
            expected_ops,
            stats.ops,
            trace_path);
    }

    for (size_t i = 0; i < num_ids; i++) {
        if (slots[i] != NULL) {
            allocator_free(slots[i]);
            slots[i] = NULL;
        }
    }
    free(slots);
    free(slot_sizes);
    free(slot_active);
    fclose(trace);
    return 0;

fail:
    for (size_t i = 0; i < num_ids; i++) {
        if (slots[i] != NULL) {
            allocator_free(slots[i]);
            slots[i] = NULL;
        }
    }
    free(slots);
    free(slot_sizes);
    free(slot_active);
    fclose(trace);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <trace.rep> [label]\n", argv[0]);
        return 1;
    }

    if (allocator_init() != 0) {
        fprintf(stderr, "allocator init failed\n");
        return 1;
    }

    const char *trace_path = argv[1];
    const char *label = argc == 3 ? argv[2] : base_name(trace_path);

    trace_stats_t stats = {0, 0, 0};
    double elapsed_ms = 0.0;
    double avg_utilization = 0.0;
    if (run_trace(trace_path, &stats, &elapsed_ms, &avg_utilization) != 0) {
        return 1;
    }

    double ops_per_sec = elapsed_ms > 0.0 ? ((double)stats.ops / (elapsed_ms / 1000.0)) : 0.0;
    double ns_per_op = stats.ops > 0 ? ((elapsed_ms * 1000000.0) / (double)stats.ops) : 0.0;

    printf(
        "%s,%s,%zu,%.3f,%.2f,%.2f,%zu,%zu,%.6f\n",
        kModeName,
        label,
        stats.ops,
        elapsed_ms,
        ops_per_sec,
        ns_per_op,
        stats.allocs,
        stats.frees,
        avg_utilization);

    return 0;
}
