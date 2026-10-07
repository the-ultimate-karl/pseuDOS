#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"

extern const uint8_t g_payload_bootx64_data[];
extern const uint64_t g_payload_bootx64_size;

extern const uint8_t g_payload_autoinit_data[];
extern const uint64_t g_payload_autoinit_size;

extern const uint8_t g_payload_xshss_data[];
extern const uint64_t g_payload_xshss_size;

extern const uint8_t g_payload_superglue_data[];
extern const uint64_t g_payload_superglue_size;

extern const uint8_t g_payload_ntfs_data[];
extern const uint64_t g_payload_ntfs_size;

extern const uint8_t g_payload_lack_data[];
extern const uint64_t g_payload_lack_size;

extern const uint8_t g_payload_ninds_data[];
extern const uint64_t g_payload_ninds_size;

extern const uint8_t g_payload_splash_data[];
extern const uint64_t g_payload_splash_size;

extern const uint8_t g_payload_gshss_data[];
extern const uint64_t g_payload_gshss_size;

extern const uint8_t g_payload_shell_data[];
extern const uint64_t g_payload_shell_size;

extern const uint8_t g_payload_sysmon_data[];
extern const uint64_t g_payload_sysmon_size;

extern const uint8_t g_payload_calc_data[];
extern const uint64_t g_payload_calc_size;

extern const uint8_t g_payload_notepad_data[];
extern const uint64_t g_payload_notepad_size;

extern const uint8_t g_payload_paint_data[];
extern const uint64_t g_payload_paint_size;

extern const uint8_t g_payload_clock_data[];
extern const uint64_t g_payload_clock_size;

extern const uint8_t g_payload_filemgr_data[];
extern const uint64_t g_payload_filemgr_size;

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

const uint8_t *payload_get_autoinit(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_autoinit_size;
    return g_payload_autoinit_data;
}

const uint8_t *payload_get_xshss(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_xshss_size;
    return g_payload_xshss_data;
}

const uint8_t *payload_get_superglue(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_superglue_size;
    return g_payload_superglue_data;
}

const uint8_t *payload_get_ntfs(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_ntfs_size;
    return g_payload_ntfs_data;
}

const uint8_t *payload_get_lack(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_lack_size;
    return g_payload_lack_data;
}

const uint8_t *payload_get_ninds(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_ninds_size;
    return g_payload_ninds_data;
}

const uint8_t *payload_get_splash(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_splash_size;
    return g_payload_splash_data;
}

const uint8_t *payload_get_gshss(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_gshss_size;
    return g_payload_gshss_data;
}

const uint8_t *payload_get_shell(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_shell_size;
    return g_payload_shell_data;
}

const uint8_t *payload_get_sysmon(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_sysmon_size;
    return g_payload_sysmon_data;
}

const uint8_t *payload_get_calc(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_calc_size;
    return g_payload_calc_data;
}

const uint8_t *payload_get_notepad(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_notepad_size;
    return g_payload_notepad_data;
}

const uint8_t *payload_get_paint(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_paint_size;
    return g_payload_paint_data;
}

const uint8_t *payload_get_clock(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_clock_size;
    return g_payload_clock_data;
}

const uint8_t *payload_get_filemgr(size_t *out_size) {
    if (out_size) *out_size = (size_t)g_payload_filemgr_size;
    return g_payload_filemgr_data;
}
