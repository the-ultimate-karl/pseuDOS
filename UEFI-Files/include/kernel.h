#ifndef KERNEL_H
#define KERNEL_H

#include "efi.h"

/* Kernel main entry point */
void kernel_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/* Interactive Kernel Shell */
void shell_run(EFI_SYSTEM_TABLE *SystemTable);

#endif /* KERNEL_H */
