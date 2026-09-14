#ifndef KERNEL_H
#define KERNEL_H

#include "bootinfo.h"

void kernel_main(BootInfo *boot_info);
void shell_init(const BootInfo *boot_info);
void shell_run(const BootInfo *boot_info);
void cmd_flash(const char *arg);
void cmd_fs(const char *arg);
void cmd_screenres(const char *arg);
void cmd_switch_target(const char *arg);
void cmd_attached_drives(const char *arg);
void cmd_devpath(const char *arg);
void cmd_proctest(void);
void cmd_halt(void);
void cmd_mem(void);

#endif /* KERNEL_H */
