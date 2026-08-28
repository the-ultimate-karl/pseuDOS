#include "drivers.h"
#include "lib.h"

void memory_print_info(EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices || !SystemTable->BootServices->GetMemoryMap) {
        console_printf("Memory information unavailable (BootServices not present)\n");
        return;
    }

    UINT8 map_buffer[16384];
    UINTN map_size = sizeof(map_buffer);
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;

    EFI_STATUS status = SystemTable->BootServices->GetMemoryMap(
        &map_size,
        (EFI_MEMORY_DESCRIPTOR *)map_buffer,
        &map_key,
        &desc_size,
        &desc_ver
    );

    if (EFI_ERROR(status)) {
        console_printf("Failed to query UEFI memory map (status: 0x%llx)\n", status);
        return;
    }

    UINT64 total_usable = 0;
    UINT64 total_boot_services = 0;
    UINT64 total_runtime_services = 0;
    UINT64 total_acpi = 0;
    UINT64 total_reserved = 0;
    UINT64 total_system_ram = 0;

    UINTN count = map_size / desc_size;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(map_buffer + (i * desc_size));
        UINT64 bytes = desc->NumberOfPages * 4096;

        switch (desc->Type) {
            case EfiConventionalMemory:
            case EfiLoaderCode:
            case EfiLoaderData:
                total_usable += bytes;
                total_system_ram += bytes;
                break;
            case EfiBootServicesCode:
            case EfiBootServicesData:
                total_boot_services += bytes;
                total_system_ram += bytes;
                break;
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
                total_runtime_services += bytes;
                total_system_ram += bytes;
                break;
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
                total_acpi += bytes;
                total_system_ram += bytes;
                break;
            case EfiReservedMemoryType:
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiUnusableMemory:
            default:
                total_reserved += bytes;
                break;
        }
    }

    console_printf("Physical Memory Statistics (UEFI Memory Map):\n");
    console_printf("  Total System RAM  : %llu MB (%llu bytes)\n", total_system_ram / (1024 * 1024), total_system_ram);
    console_printf("  Usable / Free RAM : %llu MB (%llu bytes)\n", total_usable / (1024 * 1024), total_usable);
    console_printf("  Boot Services RAM : %llu MB (%llu bytes)\n", total_boot_services / (1024 * 1024), total_boot_services);
    console_printf("  Runtime Svcs RAM  : %llu KB (%llu bytes)\n", total_runtime_services / 1024, total_runtime_services);
    console_printf("  ACPI Tables / NVS : %llu KB (%llu bytes)\n", total_acpi / 1024, total_acpi);
    console_printf("  Reserved / MMIO   : %llu MB (%llu bytes)\n", total_reserved / (1024 * 1024), total_reserved);
    console_printf("  Memory Map Descs  : %u entries (descriptor size: %u bytes)\n", (uint32_t)count, (uint32_t)desc_size);
}
