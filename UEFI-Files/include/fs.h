#ifndef FS_H
#define FS_H

#include "efi.h"

typedef struct {
    char partition_path[256];
    char hardware_path[512];
    int use_hardware_path; /* 0 = partition path, 1 = hardware path */
} BootLocationInfo;

extern BootLocationInfo g_boot_location;

/* Initialize and detect boot filepath from UEFI protocols */
EFI_STATUS fs_init_boot_location(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/* Get the current active prompt path string */
const char *fs_get_active_path(void);

/* Toggle between partition path and hardware path */
int fs_toggle_path_mode(void);

#endif /* FS_H */
