#include "drivers.h"
#include "bootinfo.h"
#include "lib.h"

/* Standard UEFI Memory Descriptor format */
typedef struct {
    uint32_t type;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} uefi_mem_desc_t;

void memory_print_info(const MemoryMapInfo *mem_info) {
    if (!mem_info || mem_info->map_buffer == 0 || mem_info->descriptor_size == 0) {
        console_printf("Memory: map information not available\n");
        return;
    }

    uint64_t total_system_bytes = 0;
    uint64_t usable_bytes = 0;
    uint64_t boot_services_bytes = 0;
    uint64_t runtime_services_bytes = 0;
    uint64_t acpi_bytes = 0;
    uint64_t pcie_mmio_bytes = 0;
    uint64_t reserved_memory_bytes = 0;
    uint32_t descriptor_count = 0;

    size_t num_entries = mem_info->map_size / mem_info->descriptor_size;
    const uint8_t *buf = (const uint8_t *)(uintptr_t)mem_info->map_buffer;

    for (size_t i = 0; i < num_entries; i++) {
        const uefi_mem_desc_t *desc = (const uefi_mem_desc_t *)(buf + (i * mem_info->descriptor_size));
        uint64_t size_bytes = desc->number_of_pages * 4096;
        descriptor_count++;

        switch (desc->type) {
            case 7: /* EfiConventionalMemory */
                usable_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case 1: /* EfiLoaderCode */
            case 2: /* EfiLoaderData */
            case 3: /* EfiBootServicesCode */
            case 4: /* EfiBootServicesData */
                boot_services_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case 5: /* EfiRuntimeServicesCode */
            case 6: /* EfiRuntimeServicesData */
                runtime_services_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case 9:  /* EfiACPIReclaimMemory */
            case 10: /* EfiACPIMemoryNVS */
                acpi_bytes += size_bytes;
                total_system_bytes += size_bytes;
                break;
            case 11: /* EfiMemoryMappedIO */
            case 12: /* EfiMemoryMappedIOPortSpace */
                pcie_mmio_bytes += size_bytes;
                break;
            case 0: /* EfiReservedMemoryType */
            case 8: /* EfiUnusableMemory */
            default:
                if (size_bytes >= (1024ULL * 1024ULL * 1024ULL)) {
                    pcie_mmio_bytes += size_bytes;
                } else {
                    reserved_memory_bytes += size_bytes;
                }
                break;
        }
    }

    console_printf("Physical Memory Statistics (UEFI Memory Map):\n");
    console_printf("  Total System RAM  : %lu MB (%lu B)\n", total_system_bytes / (1024 * 1024), total_system_bytes);
    console_printf("  Usable / Free RAM : %lu MB (%lu B)\n", usable_bytes / (1024 * 1024), usable_bytes);
    console_printf("  Boot Services RAM : %lu MB (%lu B)\n", boot_services_bytes / (1024 * 1024), boot_services_bytes);
    console_printf("  Runtime Svcs RAM  : %lu KB (%lu B)\n", runtime_services_bytes / 1024, runtime_services_bytes);
    console_printf("  ACPI Tables / NVS : %lu KB (%lu B)\n", acpi_bytes / 1024, acpi_bytes);
    if (pcie_mmio_bytes > 0) {
        console_printf("  PCIe MMIO Aperture: %lu MB (%lu B) [PCI Express Address Space]\n", pcie_mmio_bytes / (1024 * 1024), pcie_mmio_bytes);
    }
    if (reserved_memory_bytes > 0) {
        console_printf("  Reserved Memory   : %lu KB (%lu B)\n", reserved_memory_bytes / 1024, reserved_memory_bytes);
    }
    console_printf("  Memory Descriptors: %u entries (Descriptor size: %lu B)\n", descriptor_count, mem_info->descriptor_size);
    console_printf("  Kernel Heap Used  : %lu KB / %lu KB total\n", heap_get_used() / 1024, heap_get_total() / 1024);
}
