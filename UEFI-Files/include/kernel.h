#ifndef KERNEL_H
#define KERNEL_H

#include "bootinfo.h"

void kernel_main(BootInfo *boot_info);
void shell_init(const BootInfo *boot_info);
void shell_run(const BootInfo *boot_info);

#endif /* KERNEL_H */
