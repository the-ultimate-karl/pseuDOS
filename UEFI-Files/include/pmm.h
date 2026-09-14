#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

#define PAGE_SIZE 4096ULL

/* Physical Memory Manager API */
void pmm_init(const MemoryMapInfo *mem, const BootInfo *boot_info);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);
void pmm_free_page(uint64_t phys_addr);
void pmm_free_pages(uint64_t phys_addr, size_t count);

size_t pmm_get_free_pages(void);
size_t pmm_get_total_pages(void);
uint64_t pmm_get_max_phys_addr(void);

#endif /* PMM_H */
