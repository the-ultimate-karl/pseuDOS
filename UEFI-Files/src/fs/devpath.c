#include "fs.h"
#include "lib.h"

BootLocationInfo g_boot_location = {
    .base_hardware_path = "PciRoot(0x0)/Pci(0x1,0x1)/Ata(0x0)/CDROM(0x0)",
    .partition_boot_file = "\\EFI\\pseuDOS\\kernel.bin",
    .mode = DEVPATH_MODE_FIRMWARE
};

static void unicode_to_ascii(const CHAR16 *src, char *dst, size_t max_len) {
    size_t i = 0;
    if (!src || !dst || max_len == 0) return;
    while (src[i] != 0 && i + 1 < max_len) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

EFI_STATUS fs_init_boot_location(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) return EFI_INVALID_PARAMETER;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status = SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded_image
    );

    if (!EFI_ERROR(status) && loaded_image) {
        /* Extract FilePath if present */
        if (loaded_image->FilePath) {
            EFI_DEVICE_PATH_PROTOCOL *dp = loaded_image->FilePath;
            while (dp && dp->Type != EFI_DEVICE_PATH_TYPE_END) {
                if (dp->Type == EFI_DEVICE_PATH_TYPE_MEDIA && dp->SubType == 0x04) {
                    const CHAR16 *file_str = (const CHAR16 *)((const uint8_t *)dp + 4);
                    unicode_to_ascii(file_str, g_boot_location.partition_boot_file, sizeof(g_boot_location.partition_boot_file));
                    break;
                }
                uint16_t len = (uint16_t)(dp->Length[0] | (dp->Length[1] << 8));
                if (len < 4) break;
                dp = (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)dp + len);
            }
        }

        /* Check DeviceHandle for hardware device path */
        if (loaded_image->DeviceHandle) {
            EFI_DEVICE_PATH_PROTOCOL *dev_dp = NULL;
            status = SystemTable->BootServices->HandleProtocol(
                loaded_image->DeviceHandle,
                &gEfiDevicePathProtocolGuid,
                (VOID **)&dev_dp
            );

            if (!EFI_ERROR(status) && dev_dp) {
                EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dp_to_text = NULL;
                status = SystemTable->BootServices->LocateProtocol(
                    &gEfiDevicePathToTextProtocolGuid,
                    NULL,
                    (VOID **)&dp_to_text
                );

                if (!EFI_ERROR(status) && dp_to_text && dp_to_text->ConvertDevicePathToText) {
                    CHAR16 *text_u16 = dp_to_text->ConvertDevicePathToText(dev_dp, 1, 0);
                    if (text_u16) {
                        unicode_to_ascii(text_u16, g_boot_location.base_hardware_path, sizeof(g_boot_location.base_hardware_path));
                    }
                }
            }
        }
    }

    return EFI_SUCCESS;
}

void devpath_set_mode(devpath_mode_t mode) {
    g_boot_location.mode = mode;
}

devpath_mode_t devpath_get_mode(void) {
    return g_boot_location.mode;
}

void fs_get_prompt_path(char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;

    const char *cwd = vfs_getcwd();

    /* Convert cwd to DOS-style backslashes for firmware and hardware modes */
    char dos_cwd[256];
    size_t i = 0;
    while (cwd[i] != '\0' && i < sizeof(dos_cwd) - 1) {
        dos_cwd[i] = (cwd[i] == '/') ? '\\' : cwd[i];
        i++;
    }
    dos_cwd[i] = '\0';
    if (dos_cwd[0] == '\0') {
        dos_cwd[0] = '\\';
        dos_cwd[1] = '\0';
    }

    switch (g_boot_location.mode) {
        case DEVPATH_MODE_HARDWARE:
            if (strcmp(dos_cwd, "\\") == 0) {
                snprintf(out_buf, max_len, "%s\\", g_boot_location.base_hardware_path);
            } else {
                snprintf(out_buf, max_len, "%s%s", g_boot_location.base_hardware_path, dos_cwd);
            }
            break;
        case DEVPATH_MODE_SOFTWARE:
            strncpy(out_buf, cwd, max_len - 1);
            break;
        case DEVPATH_MODE_FIRMWARE:
        default:
            strncpy(out_buf, dos_cwd, max_len - 1);
            break;
    }
    out_buf[max_len - 1] = '\0';
}
