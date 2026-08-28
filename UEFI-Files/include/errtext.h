#ifndef PSEUDOS_ERRTEXT_H
#define PSEUDOS_ERRTEXT_H

#include "efi.h"

/*
 * Bootloader error codes adapting pseuDOS /protected/bootmgr/errtext.py
 * into real hardware and UEFI firmware error conditions.
 */
typedef enum {
    /* Mapped from Python ImportError: cannot mount kernel image */
    ERR_KERNEL_MOUNT_FAILED,

    /* Mapped from Python ModuleNotFoundError: No such file or directory */
    ERR_KERNEL_NOT_FOUND,

    /* Mapped from Python TypeError / NameError / SyntaxError: cannot verify integrity of the kernel */
    ERR_KERNEL_INTEGRITY,

    /* Missing real hardware / firmware errors */
    ERR_MEMORY_MAP_EXHAUSTED,
    ERR_GOP_INIT_FAILED,
    ERR_UNSUPPORTED_CPU,
    ERR_ACPI_RSDP_NOT_FOUND,
    ERR_EXIT_BOOT_SERVICES_FAILED,
    ERR_SECURE_BOOT_VIOLATION,
    ERR_STORAGE_DEVICE_IO,
    ERR_GENERIC_BOOT_FAILURE
} boot_error_t;

typedef boot_error_t BootErrorCode;

/*
 * Print formatted bootloader error diagnostic to UEFI Console and UART COM1,
 * replicating pseuDOS error_boot structure with real hardware diagnostics.
 */
void error_boot(EFI_SYSTEM_TABLE *SystemTable, boot_error_t err_code, const char *extra_info);

#endif /* PSEUDOS_ERRTEXT_H */
