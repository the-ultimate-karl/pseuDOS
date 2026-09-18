#include "shm.h"
#include "pmm.h"
#include "vmm.h"
#include "syscall.h"
#include "process.h"
#include "lib.h"
#include "klog.h"
#include "bootinfo.h"

extern const BootInfo *g_boot_info_global;

static shm_region_t g_shm_regions[MAX_SHM_REGIONS];

void shm_init(void) {
    memset(g_shm_regions, 0, sizeof(g_shm_regions));
    klog_info("Shared memory subsystem initialized (%d regions supported)", MAX_SHM_REGIONS);
}

int sys_shm_create(const char *name, size_t size) {
    if (!name || name[0] == '\0') return -EINVAL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;

    /* Check for special framebuffer region "/dev/fb0" or "framebuffer" */
    int is_fb = 0;
    if (strcmp(name, "/dev/fb0") == 0 || strcmp(name, "framebuffer") == 0) {
        is_fb = 1;
    }

    /* 1. Check if named region already exists */
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (g_shm_regions[i].in_use && strncmp(g_shm_regions[i].name, name, SHM_NAME_MAX) == 0) {
            shm_region_t *reg = &g_shm_regions[i];
            reg->ref_count++;
            int found = 0;
            for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
                if (reg->clients[c].ref_count > 0 && reg->clients[c].pid == pid) {
                    reg->clients[c].ref_count++;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
                    if (reg->clients[c].ref_count == 0) {
                        reg->clients[c].pid = pid;
                        reg->clients[c].ref_count = 1;
                        reg->clients[c].virt_addr = 0;
                        break;
                    }
                }
            }
            return reg->id;
        }
    }

    /* 2. Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (!g_shm_regions[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;

    shm_region_t *reg = &g_shm_regions[slot];
    memset(reg, 0, sizeof(shm_region_t));

    if (is_fb) {
        if (!g_boot_info_global || !g_boot_info_global->fb.physical_base || !g_boot_info_global->fb.buffer_size) {
            return -ENODEV;
        }
        reg->phys_addr = g_boot_info_global->fb.physical_base;
        reg->size = g_boot_info_global->fb.buffer_size;
        reg->num_pages = (reg->size + PAGE_SIZE - 1) / PAGE_SIZE;
        reg->is_framebuffer = 1;
    } else {
        if (size == 0) return -EINVAL;
        size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(num_pages);
        if (!phys) return -ENOMEM;

        /* Zero out shared memory */
        memset((void *)(uintptr_t)phys, 0, num_pages * PAGE_SIZE);

        reg->phys_addr = phys;
        reg->size = num_pages * PAGE_SIZE;
        reg->num_pages = num_pages;
        reg->is_framebuffer = 0;
    }

    reg->id = slot + 1;
    reg->in_use = 1;
    strncpy(reg->name, name, SHM_NAME_MAX - 1);
    reg->name[SHM_NAME_MAX - 1] = '\0';
    reg->ref_count = 1;
    reg->creator_pid = pid;

    /* Creator is initial client */
    reg->clients[0].pid = pid;
    reg->clients[0].ref_count = 1;
    reg->clients[0].virt_addr = 0;

    klog_info("Created shared memory region %d '%s' (pages=%u, phys=0x%016llX)",
              reg->id, reg->name, (unsigned int)reg->num_pages, (unsigned long long)reg->phys_addr);
    return reg->id;
}

void *sys_shm_map(int shm_id, void *addr_hint, int flags) {
    if (shm_id < 1 || shm_id > MAX_SHM_REGIONS) return NULL;
    shm_region_t *reg = &g_shm_regions[shm_id - 1];
    if (!reg->in_use) return NULL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;
    uint64_t *pml4 = (curr && curr->cr3) ? (uint64_t *)(uintptr_t)curr->cr3 : vmm_get_kernel_pml4();

    uint64_t virt;
    if (addr_hint && (((uintptr_t)addr_hint & 0xFFFULL) == 0)) {
        virt = (uint64_t)(uintptr_t)addr_hint;
    } else {
        /* Standard virtual memory mapping window for SHM: 0x0000000080000000 + (id * 16MB) */
        virt = 0x0000000080000000ULL + ((uint64_t)shm_id * 0x01000000ULL);
    }

    uint64_t pte_flags = PTE_PRESENT | PTE_USER;
    if (flags & SHM_WRITE) {
        pte_flags |= PTE_WRITABLE;
    }

    for (size_t i = 0; i < reg->num_pages; i++) {
        uint64_t v = virt + (i * PAGE_SIZE);
        uint64_t p = reg->phys_addr + (i * PAGE_SIZE);
        if (vmm_map_page(pml4, v, p, pte_flags) != 0) {
            return NULL;
        }
    }

    /* Track mapped virtual address for calling process */
    for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
        if (reg->clients[c].ref_count > 0 && reg->clients[c].pid == pid) {
            reg->clients[c].virt_addr = virt;
            break;
        }
    }

    return (void *)(uintptr_t)virt;
}

int sys_shm_unmap(void *addr) {
    if (!addr) return -EINVAL;
    uint64_t virt = (uint64_t)(uintptr_t)addr;
    if (virt & 0xFFFULL) return -EINVAL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;
    uint64_t *pml4 = (curr && curr->cr3) ? (uint64_t *)(uintptr_t)curr->cr3 : vmm_get_kernel_pml4();

    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        shm_region_t *reg = &g_shm_regions[i];
        if (!reg->in_use) continue;

        for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
            if (reg->clients[c].ref_count > 0 && reg->clients[c].pid == pid && reg->clients[c].virt_addr == virt) {
                vmm_unmap_pages(pml4, virt, reg->num_pages);
                reg->clients[c].virt_addr = 0;
                return 0;
            }
        }
    }

    return -EINVAL;
}

int sys_shm_close(int shm_id) {
    if (shm_id < 1 || shm_id > MAX_SHM_REGIONS) return -EINVAL;
    shm_region_t *reg = &g_shm_regions[shm_id - 1];
    if (!reg->in_use) return -EINVAL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;
    uint64_t *pml4 = (curr && curr->cr3) ? (uint64_t *)(uintptr_t)curr->cr3 : vmm_get_kernel_pml4();

    for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
        if (reg->clients[c].ref_count > 0 && reg->clients[c].pid == pid) {
            reg->clients[c].ref_count--;
            if (reg->clients[c].ref_count == 0 && reg->clients[c].virt_addr != 0) {
                vmm_unmap_pages(pml4, reg->clients[c].virt_addr, reg->num_pages);
                reg->clients[c].virt_addr = 0;
            }
            break;
        }
    }

    reg->ref_count--;
    if (reg->ref_count <= 0) {
        if (!reg->is_framebuffer && reg->phys_addr) {
            pmm_free_pages(reg->phys_addr, reg->num_pages);
        }
        reg->in_use = 0;
        klog_info("Closed shared memory region %d '%s'", reg->id, reg->name);
    }
    return 0;
}

void shm_cleanup_process(uint32_t pid) {
    process_t *proc = process_get_by_pid(pid);
    uint64_t *pml4 = (proc && proc->cr3) ? (uint64_t *)(uintptr_t)proc->cr3 : NULL;

    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        shm_region_t *reg = &g_shm_regions[i];
        if (!reg->in_use) continue;

        for (int c = 0; c < MAX_SHM_CLIENTS; c++) {
            if (reg->clients[c].ref_count > 0 && reg->clients[c].pid == pid) {
                int count = reg->clients[c].ref_count;
                if (reg->clients[c].virt_addr != 0 && pml4) {
                    vmm_unmap_pages(pml4, reg->clients[c].virt_addr, reg->num_pages);
                }
                reg->clients[c].ref_count = 0;
                reg->clients[c].virt_addr = 0;
                reg->ref_count -= count;

                if (reg->ref_count <= 0) {
                    if (!reg->is_framebuffer && reg->phys_addr) {
                        pmm_free_pages(reg->phys_addr, reg->num_pages);
                    }
                    reg->in_use = 0;
                    klog_info("Closed shared memory region %d '%s' (cleaned up for PID %u)",
                              reg->id, reg->name, (unsigned int)pid);
                }
                break;
            }
        }
    }
}
