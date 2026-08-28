#include "fs.h"
#include "lib.h"

const EFI_GUID gEfiLoadedImageProtocolGuid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

const EFI_GUID gEfiDevicePathProtocolGuid = {
    0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

const EFI_GUID gEfiDevicePathToTextProtocolGuid = {
    0x8B843E20, 0x8132, 0x4852, {0x90, 0xCC, 0x55, 0x1A, 0x4E, 0x4A, 0x7F, 0x1C}
};

BootLocationInfo g_boot_location = {
    .partition_path = "\\EFI\\BOOT\\BOOTX64.EFI",
    .hardware_path = "PciRoot(0x0)/Pci(0x1,0x1)/Ata(Primary,Master,0x0)/HD(1,GPT)/\\EFI\\BOOT\\BOOTX64.EFI",
    .use_hardware_path = 0
};

/* Convert Unicode CHAR16 to ASCII */
static void unicode_to_ascii(const CHAR16 *src, char *dst, size_t max_len) {
    if (!src || !dst || max_len == 0) return;
    size_t i = 0;
    while (src[i] != 0 && i + 1 < max_len) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

/* Parse a Device Path Node manually into text */
static void manual_device_path_to_text(const EFI_DEVICE_PATH_PROTOCOL *dp, char *out, size_t max_len) {
    out[0] = '\0';
    if (!dp) return;

    const EFI_DEVICE_PATH_PROTOCOL *node = dp;
    while (node && node->Type != EFI_DEVICE_PATH_TYPE_END) {
        UINT16 length = (UINT16)(node->Length[0] | (node->Length[1] << 8));
        if (length < 4) break;

        char seg[128];
        seg[0] = '\0';

        if (node->Type == EFI_DEVICE_PATH_TYPE_HARDWARE) {
            if (node->SubType == 0x01) { /* PCI */
                const UINT8 *data = (const UINT8 *)node;
                UINT8 func = data[4];
                UINT8 dev = data[5];
                snprintf(seg, sizeof(seg), "/Pci(0x%x,0x%x)", dev, func);
            }
        } else if (node->Type == EFI_DEVICE_PATH_TYPE_ACPI) {
            if (node->SubType == 0x01) { /* ACPI HID */
                snprintf(seg, sizeof(seg), "PciRoot(0x0)");
            }
        } else if (node->Type == EFI_DEVICE_PATH_TYPE_MESSAGING) {
            if (node->SubType == 0x01) { /* ATAPI */
                const UINT8 *data = (const UINT8 *)node;
                snprintf(seg, sizeof(seg), "/Ata(%s,%s,0x%x)",
                         (data[4] == 0) ? "Primary" : "Secondary",
                         (data[5] == 0) ? "Master" : "Slave",
                         data[6]);
            } else if (node->SubType == 0x17) { /* NVMe */
                snprintf(seg, sizeof(seg), "/NVMe(0x1)");
            } else if (node->SubType == 0x05) { /* USB */
                snprintf(seg, sizeof(seg), "/USB(0x0)");
            }
        } else if (node->Type == EFI_DEVICE_PATH_TYPE_MEDIA) {
            if (node->SubType == 0x01) { /* Hard Drive Partition */
                const UINT8 *data = (const UINT8 *)node;
                UINT32 part_num = *(const UINT32 *)(data + 4);
                snprintf(seg, sizeof(seg), "/HD(%u,GPT)", part_num);
            } else if (node->SubType == 0x02) { /* CD-ROM */
                snprintf(seg, sizeof(seg), "/CDROM(0x1)");
            } else if (node->SubType == 0x04) { /* File Path */
                const CHAR16 *path16 = (const CHAR16 *)((const UINT8 *)node + 4);
                char path_ascii[128];
                unicode_to_ascii(path16, path_ascii, sizeof(path_ascii));
                snprintf(seg, sizeof(seg), "/%s", path_ascii);
            }
        }

        if (seg[0] != '\0') {
            if (strlen(out) + strlen(seg) < max_len - 1) {
                strcat(out, seg);
            }
        }

        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + length);
    }
}

EFI_STATUS fs_init_boot_location(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) return EFI_INVALID_PARAMETER;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status = SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded_image
    );

    if (EFI_ERROR(status) || !loaded_image) {
        return status;
    }

    /* Extract Partition File Path from FilePath device path node */
    if (loaded_image->FilePath) {
        char fpath[256];
        manual_device_path_to_text(loaded_image->FilePath, fpath, sizeof(fpath));
        if (fpath[0] != '\0') {
            if (fpath[0] == '/') {
                /* Format as Windows/DOS backslash path */
                for (size_t i = 0; fpath[i]; i++) {
                    if (fpath[i] == '/') fpath[i] = '\\';
                }
            }
            strncpy(g_boot_location.partition_path, fpath, sizeof(g_boot_location.partition_path) - 1);
        }
    }

    /* Extract Absolute Hardware Device Path */
    char dev_text[256] = "";
    if (loaded_image->DeviceHandle) {
        EFI_DEVICE_PATH_PROTOCOL *dev_path = NULL;
        status = SystemTable->BootServices->HandleProtocol(
            loaded_image->DeviceHandle,
            &gEfiDevicePathProtocolGuid,
            (VOID **)&dev_path
        );

        if (!EFI_ERROR(status) && dev_path) {
            manual_device_path_to_text(dev_path, dev_text, sizeof(dev_text));
        }
    }

    if (dev_text[0] != '\0') {
        snprintf(g_boot_location.hardware_path, sizeof(g_boot_location.hardware_path),
                 "%s/%s", dev_text, g_boot_location.partition_path);
    } else {
        snprintf(g_boot_location.hardware_path, sizeof(g_boot_location.hardware_path),
                 "PciRoot(0x0)/Pci(0x1,0x1)/Ata(Primary,Master,0x0)/HD(1,GPT)/%s",
                 g_boot_location.partition_path);
    }

    g_boot_location.use_hardware_path = 0; /* Default to partition path */
    return EFI_SUCCESS;
}

const char *fs_get_active_path(void) {
    if (g_boot_location.use_hardware_path) {
        return g_boot_location.hardware_path;
    }
    return g_boot_location.partition_path;
}

int fs_toggle_path_mode(void) {
    g_boot_location.use_hardware_path = !g_boot_location.use_hardware_path;
    return g_boot_location.use_hardware_path;
}
