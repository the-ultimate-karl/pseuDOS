#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"

extern const uint8_t g_payload_bootx64_data[];
extern const uint64_t g_payload_bootx64_size;

extern const BootInfo *g_boot_info_global;

const uint8_t *payload_get_bootloader(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_bootx64_size;
    return g_payload_bootx64_data;
}

const uint8_t *payload_get_kernel(size_t *out_size) {
    if (g_boot_info_global && g_boot_info_global->kernel_raw_file_base && g_boot_info_global->kernel_raw_file_size) {
        if (out_size) *out_size = (size_t)g_boot_info_global->kernel_raw_file_size;
        return (const uint8_t *)(uintptr_t)g_boot_info_global->kernel_raw_file_base;
    }
    if (g_boot_info_global && g_boot_info_global->kernel_physical_base && g_boot_info_global->kernel_image_size) {
        if (out_size) *out_size = (size_t)g_boot_info_global->kernel_image_size;
        return (const uint8_t *)(uintptr_t)g_boot_info_global->kernel_physical_base;
    }
    if (out_size) *out_size = 512 * 1024;
    return (const uint8_t *)0x10000000;
}
