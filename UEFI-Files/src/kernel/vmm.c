#include "vmm.h"
#include "pmm.h"
#include "lib.h"
#include "drivers.h"

#define PAGE_SIZE_2MB (2ULL * 1024ULL * 1024ULL)

static uint64_t *g_kernel_pml4 = NULL;

static void vmm_map_2mb_range(uint64_t *pml4, uint64_t virt_base, uint64_t phys_base, uint64_t size, uint64_t flags) {
    uint64_t virt = virt_base;
    uint64_t phys = phys_base;
    uint64_t end = virt_base + size;

    while (virt < end) {
        size_t pml4_idx = (virt >> 39) & 0x1FF;
        size_t pdpt_idx = (virt >> 30) & 0x1FF;
        size_t pd_idx   = (virt >> 21) & 0x1FF;

        /* Check/Allocate PDPT */
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            uint64_t new_table = pmm_alloc_page();
            if (!new_table) return;
            memset((void *)(uintptr_t)new_table, 0, PAGE_SIZE);
            pml4[pml4_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
        }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);

        /* Check/Allocate PD */
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            uint64_t new_table = pmm_alloc_page();
            if (!new_table) return;
            memset((void *)(uintptr_t)new_table, 0, PAGE_SIZE);
            pdpt[pdpt_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
        }
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);

        /* Map 2MB Huge Page */
        pd[pd_idx] = (phys & ~(PAGE_SIZE_2MB - 1)) | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE | (flags & (PTE_GLOBAL | PTE_USER));

        virt += PAGE_SIZE_2MB;
        phys += PAGE_SIZE_2MB;
    }
}

int vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return -1;

    size_t pml4_idx = (virt >> 39) & 0x1FF;
    size_t pdpt_idx = (virt >> 30) & 0x1FF;
    size_t pd_idx   = (virt >> 21) & 0x1FF;
    size_t pt_idx   = (virt >> 12) & 0x1FF;

    /* 1. Walk PML4 */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_table = pmm_alloc_page();
        if (!new_table) return -1;
        memset((void *)(uintptr_t)new_table, 0, PAGE_SIZE);
        pml4[pml4_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (flags & PTE_USER) {
        pml4[pml4_idx] |= (PTE_USER | PTE_WRITABLE);
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);

    /* 2. Walk PDPT */
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_table = pmm_alloc_page();
        if (!new_table) return -1;
        memset((void *)(uintptr_t)new_table, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (flags & PTE_USER) {
        pdpt[pdpt_idx] |= (PTE_USER | PTE_WRITABLE);
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);

    /* 3. Walk PD */
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_table = pmm_alloc_page();
        if (!new_table) return -1;
        memset((void *)(uintptr_t)new_table, 0, PAGE_SIZE);
        pd[pd_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (pd[pd_idx] & PTE_HUGE) {
        /* Split 2MB page if already mapped as huge */
        uint64_t old_phys_base = pd[pd_idx] & ~(PAGE_SIZE_2MB - 1);
        uint64_t old_flags = pd[pd_idx] & 0xFFFULL;
        uint64_t new_table = pmm_alloc_page();
        if (!new_table) return -1;
        uint64_t *new_pt = (uint64_t *)(uintptr_t)new_table;
        for (size_t i = 0; i < 512; i++) {
            new_pt[i] = (old_phys_base + i * PAGE_SIZE) | (old_flags & ~PTE_HUGE) | PTE_PRESENT;
        }
        pd[pd_idx] = new_table | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (flags & PTE_USER) {
        pd[pd_idx] |= (PTE_USER | PTE_WRITABLE);
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);

    /* 4. Set PTE */
    pt[pt_idx] = (phys & ~0xFFFULL) | (flags & 0xFFFULL) | PTE_PRESENT;

    /* Invalidate TLB for virtual address */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");

    return 0;
}

int vmm_map_pages(uint64_t *pml4, uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
    for (size_t i = 0; i < count; i++) {
        if (vmm_map_page(pml4, virt + (i * PAGE_SIZE), phys + (i * PAGE_SIZE), flags) != 0) {
            return -1;
        }
    }
    return 0;
}

uint64_t *vmm_create_user_address_space(void) {
    if (!g_kernel_pml4) return NULL;

    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return NULL;

    uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_phys;

    /* Zero out user half (entries 0..255) */
    memset(new_pml4, 0, 256 * sizeof(uint64_t));

    /* Clone kernel higher-half (entries 256..511) */
    memcpy(&new_pml4[256], &g_kernel_pml4[256], 256 * sizeof(uint64_t));

    /* Also keep identity mapping in entry 0 for smooth transitions */
    new_pml4[0] = g_kernel_pml4[0] | PTE_USER | PTE_WRITABLE;

    return new_pml4;
}

void vmm_switch_address_space(uint64_t *pml4) {
    if (!pml4) return;
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)pml4) : "memory");
}

uint64_t *vmm_get_kernel_pml4(void) {
    return g_kernel_pml4;
}

void vmm_init(const BootInfo *boot_info) {
    /* 1. Allocate Master Kernel PML4 */
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) {
        return;
    }

    g_kernel_pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    memset(g_kernel_pml4, 0, PAGE_SIZE);

    /* 2. Determine Maximum Physical Memory to Map */
    uint64_t max_ram = pmm_get_max_phys_addr();
    if (max_ram < (4ULL * 1024ULL * 1024ULL * 1024ULL)) {
        max_ram = (4ULL * 1024ULL * 1024ULL * 1024ULL); /* At least 4 GB */
    }

    /* Check if Framebuffer extends beyond standard RAM */
    if (boot_info && boot_info->fb.physical_base && boot_info->fb.buffer_size) {
        uint64_t fb_end = boot_info->fb.physical_base + boot_info->fb.buffer_size;
        if (fb_end > max_ram) {
            max_ram = (fb_end + PAGE_SIZE_2MB - 1) & ~(PAGE_SIZE_2MB - 1);
        }
    }

    /* Round up to 2MB boundary */
    max_ram = (max_ram + PAGE_SIZE_2MB - 1) & ~(PAGE_SIZE_2MB - 1);

    /* 3. Identity Map Physical Memory (Lower Half: 0x0000000000000000+) */
    vmm_map_2mb_range(g_kernel_pml4, 0x0000000000000000ULL, 0x0000000000000000ULL, max_ram, 0);

    /* 4. Map Kernel Higher-Half (0xFFFF800000000000+) */
    vmm_map_2mb_range(g_kernel_pml4, KERNEL_VIRTUAL_BASE, 0x0000000000000000ULL, max_ram, PTE_GLOBAL);

    /* 5. Switch to new Master Kernel Page Directory */
    vmm_switch_address_space(g_kernel_pml4);
}
