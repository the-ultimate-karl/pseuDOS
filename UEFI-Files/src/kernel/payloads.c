#include "storage.h"
#include "bootinfo.h"
#include "fs.h"
#include "lib.h"

static const uint8_t *get_vfs_payload_content(const char *path, size_t *out_size) {
    vfs_node_t *node = vfs_find_node(path);
    if (node && node->size > 0 && node->content) {
        if (out_size) *out_size = node->size;
        return (const uint8_t *)node->content;
    }
    if (out_size) *out_size = 0;
    return NULL;
}

const uint8_t *payload_get_bootloader(size_t *out_size) {
    return get_vfs_payload_content("/EFI/BOOT/BOOTX64.EFI", out_size);
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
    return get_vfs_payload_content("/protected/krnl/vpkernel", out_size);
}

const uint8_t *payload_get_autoinit(size_t *out_size) {
    return get_vfs_payload_content("/protected/krnl/autoinit.exe", out_size);
}

const uint8_t *payload_get_xshss(size_t *out_size) {
    return get_vfs_payload_content("/protected/crit/xshss.exe", out_size);
}

const uint8_t *payload_get_gman(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/gman.exe", out_size);
}

const uint8_t *payload_get_superglue(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/superglue.exe", out_size);
}

const uint8_t *payload_get_ntfs(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/ntfs.exe", out_size);
}

const uint8_t *payload_get_lack(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/lack.exe", out_size);
}

const uint8_t *payload_get_ninds(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/ninds.exe", out_size);
}

const uint8_t *payload_get_splash(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/splash.exe", out_size);
}

const uint8_t *payload_get_gshss(size_t *out_size) {
    return get_vfs_payload_content("/protected/gui/gshss.exe", out_size);
}

const uint8_t *payload_get_shell(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/shell.exe", out_size);
}

const uint8_t *payload_get_sysmon(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/sysmon.exe", out_size);
}

const uint8_t *payload_get_calc(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/calc.exe", out_size);
}

const uint8_t *payload_get_notepad(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/notepad.exe", out_size);
}

const uint8_t *payload_get_paint(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/paint.exe", out_size);
}

const uint8_t *payload_get_clock(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/clock.exe", out_size);
}

const uint8_t *payload_get_filemgr(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/filemgr.exe", out_size);
}

const uint8_t *payload_get_view(size_t *out_size) {
    return get_vfs_payload_content("/protected/apps/view.exe", out_size);
}

