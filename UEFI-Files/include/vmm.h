#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

/* Higher-Half Canonical Kernel Virtual Base (PML4 Index 256) */
#define KERNEL_VIRTUAL_BASE 0xFFFF800000000000ULL

/* Page Table Entry Flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)  /* 2MB or 1GB page */
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

/* Virtual Memory Manager API */
void vmm_init(const BootInfo *boot_info);
int vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_map_pages(uint64_t *pml4, uint64_t virt, uint64_t phys, size_t count, uint64_t flags);
uint64_t *vmm_create_user_address_space(void);
void vmm_switch_address_space(uint64_t *pml4);
uint64_t *vmm_get_kernel_pml4(void);

#endif /* VMM_H */
