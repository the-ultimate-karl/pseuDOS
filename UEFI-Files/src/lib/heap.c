#include "lib.h"

typedef struct heap_block {
    size_t size;               /* Size of user data area in bytes */
    int is_free;               /* 1 if free, 0 if allocated */
    struct heap_block *next;   /* Next block in memory order */
    struct heap_block *prev;   /* Previous block in memory order */
} heap_block_t;

#define BLOCK_HEADER_SIZE (sizeof(heap_block_t))
#define ALIGN8(x) (((x) + 7) & ~7)

static uint8_t *g_heap_start = NULL;
static size_t g_heap_total_size = 0;
static heap_block_t *g_first_block = NULL;

EFI_STATUS heap_init(EFI_SYSTEM_TABLE *SystemTable, size_t initial_bytes) {
    if (!SystemTable || !SystemTable->BootServices || !SystemTable->BootServices->AllocatePages) {
        return EFI_INVALID_PARAMETER;
    }

    /* Round up to 4KB pages (e.g. 16MB = 4096 pages) */
    UINTN pages = (initial_bytes + 4095) / 4096;
    if (pages < 256) pages = 256; /* Minimum 1MB */

    EFI_PHYSICAL_ADDRESS phys_addr = 0;
    EFI_STATUS status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderData,
        pages,
        &phys_addr
    );

    if (EFI_ERROR(status) || phys_addr == 0) {
        return status;
    }

    g_heap_start = (uint8_t *)(uintptr_t)phys_addr;
    g_heap_total_size = pages * 4096;

    /* Initialize the single massive free block spanning the whole pool */
    g_first_block = (heap_block_t *)g_heap_start;
    g_first_block->size = g_heap_total_size - BLOCK_HEADER_SIZE;
    g_first_block->is_free = 1;
    g_first_block->next = NULL;
    g_first_block->prev = NULL;

    return EFI_SUCCESS;
}

void *kmalloc(size_t size) {
    if (size == 0 || !g_first_block) return NULL;

    size_t actual_size = ALIGN8(size);
    heap_block_t *curr = g_first_block;

    while (curr) {
        if (curr->is_free && curr->size >= actual_size) {
            /* Check if block can be split */
            if (curr->size >= actual_size + BLOCK_HEADER_SIZE + 16) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)curr + BLOCK_HEADER_SIZE + actual_size);
                new_block->size = curr->size - actual_size - BLOCK_HEADER_SIZE;
                new_block->is_free = 1;
                new_block->next = curr->next;
                new_block->prev = curr;

                if (curr->next) {
                    curr->next->prev = new_block;
                }
                curr->next = new_block;
                curr->size = actual_size;
            }

            curr->is_free = 0;
            return (void *)((uint8_t *)curr + BLOCK_HEADER_SIZE);
        }
        curr = curr->next;
    }

    return NULL; /* Out of memory */
}

void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr || !g_first_block) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
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

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
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
    if (!g_first_block) return 0;
    size_t used = 0;
    heap_block_t *curr = g_first_block;
    while (curr) {
        if (!curr->is_free) {
            used += curr->size + BLOCK_HEADER_SIZE;
        }
        curr = curr->next;
    }
    return used;
}

size_t heap_get_free(void) {
    if (!g_first_block) return 0;
    size_t free_mem = 0;
    heap_block_t *curr = g_first_block;
    while (curr) {
        if (curr->is_free) {
            free_mem += curr->size;
        }
        curr = curr->next;
    }
    return free_mem;
}
