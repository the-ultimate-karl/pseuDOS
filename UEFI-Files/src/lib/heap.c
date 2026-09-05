#include "lib.h"

typedef struct heap_block {
    size_t size;               /* Size of payload data in bytes */
    int is_free;               /* 1 if free, 0 if allocated */
    struct heap_block *next;   /* Next block pointer */
    struct heap_block *prev;   /* Previous block pointer */
} heap_block_t;

#define BLOCK_HEADER_SIZE (sizeof(heap_block_t))

static uint8_t *g_heap_start = NULL;
static size_t g_heap_total_size = 0;
static heap_block_t *g_free_list = NULL;

void heap_init(uint64_t heap_start, size_t heap_size) {
    if (heap_start == 0 || heap_size < (BLOCK_HEADER_SIZE + 1024)) return;

    g_heap_start = (uint8_t *)(uintptr_t)heap_start;
    g_heap_total_size = heap_size;

    g_free_list = (heap_block_t *)g_heap_start;
    g_free_list->size = heap_size - BLOCK_HEADER_SIZE;
    g_free_list->is_free = 1;
    g_free_list->next = NULL;
    g_free_list->prev = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0 || !g_free_list) return NULL;

    /* Align to 16 bytes */
    size = (size + 15) & ~15;

    heap_block_t *curr = g_free_list;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            /* Can we split this block? */
            if (curr->size >= size + BLOCK_HEADER_SIZE + 32) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)curr + BLOCK_HEADER_SIZE + size);
                new_block->size = curr->size - size - BLOCK_HEADER_SIZE;
                new_block->is_free = 1;
                new_block->next = curr->next;
                new_block->prev = curr;

                if (curr->next) {
                    curr->next->prev = new_block;
                }
                curr->next = new_block;
                curr->size = size;
            }

            curr->is_free = 0;
            return (void *)((uint8_t *)curr + BLOCK_HEADER_SIZE);
        }
        curr = curr->next;
    }

    return NULL;
}

void kfree(void *ptr) {
    if (!ptr || !g_heap_start) return;

    /* Bounds check: pointer must be within valid heap range */
    uint8_t *p = (uint8_t *)ptr;
    if (p < g_heap_start + BLOCK_HEADER_SIZE || p >= g_heap_start + g_heap_total_size) {
        return;
    }

    heap_block_t *block = (heap_block_t *)(p - BLOCK_HEADER_SIZE);
    block->is_free = 1;

    /* Coalesce with next block if free */
    if (block->next && block->next->is_free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    /* Coalesce with previous block if free */
    if (block->prev && block->prev->is_free) {
        block->prev->size += BLOCK_HEADER_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

void *kcalloc(size_t num, size_t size) {
    if (num > 0 && size > (size_t)-1 / num) {
        return NULL;
    }
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    uint8_t *p = (uint8_t *)ptr;
    if (!g_heap_start || p < g_heap_start + BLOCK_HEADER_SIZE || p >= g_heap_start + g_heap_total_size) {
        return NULL;
    }

    heap_block_t *block = (heap_block_t *)(p - BLOCK_HEADER_SIZE);
    if (block->size >= new_size) {
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

size_t heap_get_total(void) {
    return g_heap_total_size;
}

size_t heap_get_used(void) {
    if (!g_heap_start) return 0;
    size_t used = 0;
    heap_block_t *curr = (heap_block_t *)g_heap_start;
    while (curr) {
        if (!curr->is_free) {
            used += curr->size + BLOCK_HEADER_SIZE;
        }
        curr = curr->next;
    }
    return used;
}
