#ifndef PSEUDOS_KERNEL_H
#define PSEUDOS_KERNEL_H

#include "efi.h"

/*
 * Kernel entry point called from the UEFI bootloader.
 */
void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

#endif /* PSEUDOS_KERNEL_H */
