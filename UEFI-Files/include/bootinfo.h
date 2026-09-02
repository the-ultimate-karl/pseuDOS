#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>
#include <stddef.h>

/* Pixel Formats */
typedef enum {
    FB_FORMAT_RGB = 0,
    FB_FORMAT_BGR = 1,
    FB_FORMAT_BITMASK = 2
} fb_pixel_format_t;

/* Framebuffer Metadata */
typedef struct {
    uint64_t physical_base;
    uint64_t buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    fb_pixel_format_t pixel_format;
} FramebufferInfo;

/* Memory Map Metadata */
typedef struct {
    uint64_t map_buffer;
    uint64_t map_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;
    uint64_t total_memory_bytes;
    uint64_t heap_physical_start;
    uint64_t heap_size_bytes;
} MemoryMapInfo;

/* Main Handoff Structure passed to kernel_main */
typedef struct {
    uint32_t magic; /* 0x50534555 "PSEU" */
    uint32_t version;

    FramebufferInfo fb;
    MemoryMapInfo   mem;

    uint64_t acpi_rsdp_address;
    uint64_t smbios_address;

    uint64_t kernel_physical_base;
    uint64_t kernel_image_size;

    char hardware_devpath[384];
    char boot_file_path[128];
} BootInfo;

#define BOOTINFO_MAGIC 0x50534555 /* "PSEU" */

#endif /* BOOTINFO_H */
