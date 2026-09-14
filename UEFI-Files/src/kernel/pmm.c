#include "pmm.h"
#include "lib.h"
#include "drivers.h"

/* UEFI Memory Descriptor Structure */
typedef struct {
    uint32_t type;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} uefi_mem_desc_t;

static uint8_t *g_pmm_bitmap = NULL;
static size_t g_total_pages = 0;
static size_t g_free_pages = 0;
static uint64_t g_max_phys_addr = 0;

static inline void bitmap_set(size_t page) {
    g_pmm_bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(size_t page) {
    g_pmm_bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(size_t page) {
    return (g_pmm_bitmap[page / 8] & (1 << (page % 8))) != 0;
}

static void pmm_reserve_range(uint64_t base, uint64_t size) {
    if (size == 0 || g_total_pages == 0) return;

    size_t start_page = base / PAGE_SIZE;
    size_t end_page = (base + size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (end_page > g_total_pages) {
        end_page = g_total_pages;
    }

    for (size_t p = start_page; p < end_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            if (g_free_pages > 0) g_free_pages--;
        }
    }
}

void pmm_init(const MemoryMapInfo *mem, const BootInfo *boot_info) {
    if (!mem || mem->map_buffer == 0 || mem->descriptor_size == 0) {
        return;
    }

    /* 1. Calculate Maximum Physical Address */
    size_t num_entries = mem->map_size / mem->descriptor_size;
    const uint8_t *buf = (const uint8_t *)(uintptr_t)mem->map_buffer;

    g_max_phys_addr = 0;
    for (size_t i = 0; i < num_entries; i++) {
        const uefi_mem_desc_t *desc = (const uefi_mem_desc_t *)(buf + (i * mem->descriptor_size));
        /* Only consider physical RAM types (exclude reserved MMIO apertures) */
        if (desc->type >= 1 && desc->type <= 7) {
            uint64_t end = desc->physical_start + (desc->number_of_pages * PAGE_SIZE);
            if (end > g_max_phys_addr) {
                g_max_phys_addr = end;
            }
        }
    }

    /* Round up to at least 1 GB to cover standard system memory */
    if (g_max_phys_addr < (1024ULL * 1024ULL * 1024ULL)) {
        g_max_phys_addr = 1024ULL * 1024ULL * 1024ULL;
    }

    g_total_pages = g_max_phys_addr / PAGE_SIZE;
    size_t bitmap_size_bytes = (g_total_pages + 7) / 8;

    /* 2. Allocate Bitmap from dynamic kernel heap */
    g_pmm_bitmap = (uint8_t *)kmalloc(bitmap_size_bytes);
    if (!g_pmm_bitmap) {
        return;
    }

    /* Initially mark all pages as used/reserved */
    memset(g_pmm_bitmap, 0xFF, bitmap_size_bytes);
    g_free_pages = 0;

    /* 3. Mark EfiConventionalMemory (Type 7) as free */
    for (size_t i = 0; i < num_entries; i++) {
        const uefi_mem_desc_t *desc = (const uefi_mem_desc_t *)(buf + (i * mem->descriptor_size));
        if (desc->type == 7) { /* EfiConventionalMemory */
            size_t start = desc->physical_start / PAGE_SIZE;
            size_t count = desc->number_of_pages;
            for (size_t p = start; p < start + count && p < g_total_pages; p++) {
                if (bitmap_test(p)) {
                    bitmap_clear(p);
                    g_free_pages++;
                }
            }
        }
    }

    /* 4. Strictly Reserve Hardware, Kernel, and Subsystem Memory */
    /* Reserve first 1 MB (BIOS, IVT, BDA, EBDA, Video buffer) */
    pmm_reserve_range(0x00000000ULL, 0x00100000ULL);

    /* Reserve Kernel Image */
    if (boot_info && boot_info->kernel_physical_base && boot_info->kernel_image_size) {
        pmm_reserve_range(boot_info->kernel_physical_base, boot_info->kernel_image_size);
    }

    /* Reserve Pristine Raw Kernel File Payload */
    if (boot_info && boot_info->kernel_raw_file_base && boot_info->kernel_raw_file_size) {
        pmm_reserve_range(boot_info->kernel_raw_file_base, boot_info->kernel_raw_file_size);
    }

    /* Reserve Kernel Dynamic Heap */
    if (mem->heap_physical_start && mem->heap_size_bytes) {
        pmm_reserve_range(mem->heap_physical_start, mem->heap_size_bytes);
    }

    /* Reserve Framebuffer VRAM Aperture */
    if (boot_info && boot_info->fb.physical_base && boot_info->fb.buffer_size) {
        pmm_reserve_range(boot_info->fb.physical_base, boot_info->fb.buffer_size);
    }

    /* Reserve UEFI Memory Map Snapshot */
    if (mem->map_buffer && mem->map_size) {
        pmm_reserve_range(mem->map_buffer, mem->map_size);
    }

    /* Reserve ACPI RSDP Table */
    if (boot_info && boot_info->acpi_rsdp_address) {
        pmm_reserve_range(boot_info->acpi_rsdp_address, 4096);
    }

    /* Reserve BootInfo handoff structure */
    if (boot_info) {
        pmm_reserve_range((uint64_t)(uintptr_t)boot_info, sizeof(BootInfo));
    }
}

uint64_t pmm_alloc_page(void) {
    if (!g_pmm_bitmap || g_free_pages == 0) return 0;

    size_t words = g_total_pages / 64;
    uint64_t *bitmap64 = (uint64_t *)g_pmm_bitmap;

    for (size_t w = 0; w < words; w++) {
        if (bitmap64[w] != 0xFFFFFFFFFFFFFFFFULL) {
            /* Found a 64-page chunk with at least one free page */
            for (size_t b = 0; b < 64; b++) {
                size_t page = w * 64 + b;
                if (page >= g_total_pages) return 0;
                if (!bitmap_test(page)) {
                    bitmap_set(page);
                    g_free_pages--;
                    return (uint64_t)page * PAGE_SIZE;
                }
            }
        }
    }

    /* Handle remaining tail pages */
    for (size_t p = words * 64; p < g_total_pages; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            g_free_pages--;
            return (uint64_t)p * PAGE_SIZE;
        }
    }

    return 0;
}

uint64_t pmm_alloc_pages(size_t count) {
    if (!g_pmm_bitmap || count == 0 || g_free_pages < count) return 0;

    size_t consecutive = 0;
    size_t start_page = 0;

    for (size_t p = 0; p < g_total_pages; p++) {
        if (!bitmap_test(p)) {
            if (consecutive == 0) {
                start_page = p;
            }
            consecutive++;
            if (consecutive == count) {
                for (size_t j = start_page; j < start_page + count; j++) {
                    bitmap_set(j);
                }
                g_free_pages -= count;
                return (uint64_t)start_page * PAGE_SIZE;
            }
        } else {
            consecutive = 0;
        }
    }

    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    if (!g_pmm_bitmap || phys_addr >= g_max_phys_addr) return;

    size_t page = phys_addr / PAGE_SIZE;
    if (page < g_total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        g_free_pages++;
    }
}

void pmm_free_pages(uint64_t phys_addr, size_t count) {
    if (!g_pmm_bitmap || phys_addr >= g_max_phys_addr || count == 0) return;

    size_t start_page = phys_addr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        size_t p = start_page + i;
        if (p < g_total_pages && bitmap_test(p)) {
            bitmap_clear(p);
            g_free_pages++;
        }
    }
}

size_t pmm_get_free_pages(void) {
    return g_free_pages;
}

size_t pmm_get_total_pages(void) {
    return g_total_pages;
}

uint64_t pmm_get_max_phys_addr(void) {
    return g_max_phys_addr;
}
