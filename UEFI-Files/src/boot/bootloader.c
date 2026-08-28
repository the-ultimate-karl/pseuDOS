#include "efi.h"
#include "errtext.h"
#include "kernel.h"
#include "io.h"

/*
 * Check basic x86_64 CPU features using CPUID instruction
 */
static int check_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check maximum extended function */
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000));
    if (eax < 0x80000001) {
        return 0;
    }

    /* Check for Long Mode (bit 29 of EDX in 0x80000001) */
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001));
    if (!(edx & (1 << 29))) {
        return 0; /* Long mode not supported */
    }

    return 1;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    if (!SystemTable || !SystemTable->BootServices) {
        /* Fatal firmware state */
        error_boot(NULL, ERR_GENERIC_BOOT_FAILURE, "SystemTable or BootServices pointer is NULL");
    }

    /* Disable UEFI watchdog timer so firmware does not reset the machine */
    if (SystemTable->BootServices->SetWatchdogTimer) {
        SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    }

    /* Verify CPU long mode support */
    if (!check_cpu_features()) {
        error_boot(SystemTable, ERR_UNSUPPORTED_CPU, "x86_64 Long Mode (64-bit) feature flag not present in CPUID");
    }

    /* Clear screen and reset console if available */
    if (SystemTable->ConOut) {
        if (SystemTable->ConOut->ClearScreen) {
            SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
        }
    }

    /* Jump to kernel */
    kernel_main(SystemTable);

    /* Should never reach here */
    error_boot(SystemTable, ERR_GENERIC_BOOT_FAILURE, "Kernel unexpectedly returned to bootloader");

    return EFI_SUCCESS;
}
