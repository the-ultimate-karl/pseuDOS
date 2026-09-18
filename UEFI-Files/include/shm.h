#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stddef.h>

#define MAX_SHM_REGIONS 32
#define SHM_NAME_MAX    32

#define SHM_READ  0x01
#define SHM_WRITE 0x02

#define MAX_SHM_CLIENTS 16

typedef struct {
    uint32_t pid;
    uint64_t virt_addr;
    int ref_count;
} shm_client_ref_t;

typedef struct {
    int id;
    int in_use;
    char name[SHM_NAME_MAX];
    uint64_t phys_addr;
    size_t size;
    size_t num_pages;
    int ref_count;
    uint32_t creator_pid;
    int is_framebuffer;
    shm_client_ref_t clients[MAX_SHM_CLIENTS];
} shm_region_t;

void shm_init(void);
int sys_shm_create(const char *name, size_t size);
void *sys_shm_map(int shm_id, void *addr_hint, int flags);
int sys_shm_unmap(void *addr);
int sys_shm_close(int shm_id);
void shm_cleanup_process(uint32_t pid);

#endif /* SHM_H */
