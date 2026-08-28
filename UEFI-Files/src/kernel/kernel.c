#include "kernel.h"
#include "drivers.h"
#include "fs.h"
#include "lib.h"

EFI_STATUS EFIAPI kernel_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) {
        return EFI_INVALID_PARAMETER;
    }

    /* 1. Initialize Dynamic Memory Heap (16MB) */
    heap_init(SystemTable, 16 * 1024 * 1024);

    /* 2. Initialize Console & Keyboard */
    console_init(SystemTable);
    keyboard_init(SystemTable);

    /* 3. Discover boot location and physical device path */
    fs_init_boot_location(ImageHandle, SystemTable);

    /* 4. Initialize Virtual File System with physical storage volume integration */
    vfs_init(ImageHandle, SystemTable);

    /* 5. Initialize & Launch Interactive Kernel Shell */
    shell_init(SystemTable);
    shell_run(SystemTable);

    return EFI_SUCCESS;
}
