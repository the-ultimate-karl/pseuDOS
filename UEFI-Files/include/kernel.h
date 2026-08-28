#ifndef KERNEL_H
#define KERNEL_H

#include "efi.h"

/* Kernel main entry point */
EFI_STATUS EFIAPI kernel_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/* Interactive Kernel Shell */
void shell_init(EFI_SYSTEM_TABLE *SystemTable);
void shell_run(EFI_SYSTEM_TABLE *SystemTable);

#endif /* KERNEL_H */
