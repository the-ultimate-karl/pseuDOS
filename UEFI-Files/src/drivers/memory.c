#include "drivers.h"
#include "lib.h"

void memory_print_info(EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) {
        console_printf("memory: boot services not available\n");
        return;
    }

    UINTN memory_map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;

    /* Query buffer size needed */
    EFI_STATUS status = SystemTable->BootServices->GetMemoryMap(
        &memory_map_size,
        NULL,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    if (status != EFI_BUFFER_TOO_SMALL && !EFI_ERROR(status)) {
        console_printf("memory: failed to query memory map size\n");
        return;
    }

    /* Allocate buffer with extra margin for memory allocation itself */
    memory_map_size += 2 * descriptor_size;
    uint8_t *map_buf = (uint8_t *)kmalloc(memory_map_size);
    if (!map_buf) {
        console_printf("memory: failed to allocate temporary buffer for memory map\n");
        return;
    }

    status = SystemTable->BootServices->GetMemoryMap(
        &memory_map_size,
        (EFI_MEMORY_DESCRIPTOR *)map_buf,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    if (EFI_ERROR(status)) {
        console_printf("memory: GetMemoryMap failed (status: 0x%lx)\n", (unsigned long)status);
        kfree(map_buf);
        return;
    }

    uint64_t total_system_bytes = 0;
    uint64_t usable_bytes = 0;
    uint64_t boot_services_bytes = 0;
    uint64_t runtime_services_bytes = 0;
    uint64_t acpi_bytes = 0;
    uint64_t reserved_mmio_bytes = 0;
    uint32_t descriptor_count = 0;

    size_t num_entries = memory_map_size / descriptor_size;
    for (size_t i = 0; i < num_entries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(map_buf + (i * descriptor_size));
        uint64_t size_bytes = desc->NumberOfPages * 4096;
        descriptor_count++;

        switch (desc->Type) {
            case EfiConventionalMemory:
                usable_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
                boot_services_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
                runtime_services_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
                acpi_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case EfiReservedMemoryType:
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiUnusableMemory:
            default:
                reserved_mmio_bytes += size_bytes;
                break;
        }
    }

    kfree(map_buf);

    console_printf("physical memory statistics (uefi memory map):\n");
    console_printf("  total system ram  : %lu mb (%lu bytes)\n", total_system_bytes / (1024 * 1024), total_system_bytes);
    console_printf("  usable / free ram : %lu mb (%lu bytes)\n", usable_bytes / (1024 * 1024), usable_bytes);
    console_printf("  boot services ram : %lu mb (%lu bytes)\n", boot_services_bytes / (1024 * 1024), boot_services_bytes);
    console_printf("  runtime svcs ram  : %lu kb (%lu bytes)\n", runtime_services_bytes / 1024, runtime_services_bytes);
    console_printf("  acpi tables / nvs : %lu kb (%lu bytes)\n", acpi_bytes / 1024, acpi_bytes);
    console_printf("  reserved / mmio   : %lu mb (%lu bytes)\n", reserved_mmio_bytes / (1024 * 1024), reserved_mmio_bytes);
    console_printf("  memory map descs  : %u entries (descriptor size: %lu bytes)\n", descriptor_count, descriptor_size);
    console_printf("  kernel heap used  : %lu kb / %lu kb total\n", heap_get_used() / 1024, heap_get_total() / 1024);
}
