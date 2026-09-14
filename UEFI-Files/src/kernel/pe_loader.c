#include "pe_loader.h"
#include "fs.h"
#include "lib.h"
#include "klog.h"
#include "vmm.h"
#include "syscall.h"

int pe_load_binary(const uint8_t *raw_file, size_t raw_size, void **out_image_base, uint64_t *out_entry_point, size_t *out_image_size) {
    if (!raw_file || raw_size < 0x40 || !out_image_base || !out_entry_point) {
        return -EINVAL;
    }

    /* 1. Validate DOS MZ Header */
    if (raw_file[0] != 'M' || raw_file[1] != 'Z') {
        klog_warn("PE loader: missing 'MZ' header signature");
        return -EINVAL;
    }

    uint32_t pe_offset = *(const uint32_t *)(raw_file + 0x3C);
    if (pe_offset + 0x100 > raw_size ||
        raw_file[pe_offset] != 'P' || raw_file[pe_offset + 1] != 'E' ||
        raw_file[pe_offset + 2] != '\0' || raw_file[pe_offset + 3] != '\0') {
        klog_warn("PE loader: missing or corrupted 'PE' signature at offset 0x%X", pe_offset);
        return -EINVAL;
    }

    /* 2. Validate Architecture */
    uint16_t machine = *(const uint16_t *)(raw_file + pe_offset + 4);
    if (machine != 0x8664) {
        klog_warn("PE loader: unsupported machine type 0x%04X (expected x86_64 0x8664)", machine);
        return -EINVAL;
    }

    uint16_t num_sections = *(const uint16_t *)(raw_file + pe_offset + 6);
    uint16_t opt_hdr_size = *(const uint16_t *)(raw_file + pe_offset + 20);
    uint32_t opt_offset = pe_offset + 24;

    if (opt_offset + opt_hdr_size > raw_size || num_sections == 0) {
        klog_warn("PE loader: invalid optional header size or zero sections");
        return -EINVAL;
    }

    uint32_t entry_rva = *(const uint32_t *)(raw_file + opt_offset + 16);
    uint64_t image_base = *(const uint64_t *)(raw_file + opt_offset + 24);
    uint32_t size_of_image = *(const uint32_t *)(raw_file + opt_offset + 56);
    uint32_t size_of_headers = *(const uint32_t *)(raw_file + opt_offset + 60);

    if (size_of_image < 4096 || entry_rva == 0 || entry_rva >= size_of_image) {
        klog_warn("PE loader: invalid SizeOfImage (%u) or entry point RVA (0x%X)", size_of_image, entry_rva);
        return -EINVAL;
    }

    /* 3. Allocate In-Memory Execution Buffer */
    void *image_buffer = kmalloc(size_of_image);
    if (!image_buffer) {
        klog_err("PE loader: failed to allocate %u bytes for execution image", size_of_image);
        return -ENOMEM;
    }
    memset(image_buffer, 0, size_of_image);

    /* 4. Copy PE Headers */
    size_t copy_hdr = (size_of_headers > raw_size) ? raw_size : size_of_headers;
    memcpy(image_buffer, raw_file, copy_hdr);

    /* 5. Copy Sections to Virtual Addresses */
    uint32_t sec_table_offset = opt_offset + opt_hdr_size;
    const uint8_t *sec_hdr = raw_file + sec_table_offset;

    for (uint16_t i = 0; i < num_sections; i++) {
        uint32_t vaddr = *(const uint32_t *)(sec_hdr + 12);
        uint32_t raw_data_size = *(const uint32_t *)(sec_hdr + 16);
        uint32_t raw_data_ptr = *(const uint32_t *)(sec_hdr + 20);

        if (raw_data_size > 0 && raw_data_ptr > 0) {
            if (raw_data_ptr + raw_data_size <= raw_size && vaddr + raw_data_size <= size_of_image) {
                memcpy((uint8_t *)image_buffer + vaddr, raw_file + raw_data_ptr, raw_data_size);
            }
        }
        sec_hdr += 40;
    }

    /* 6. Apply PE Base Relocations */
    int64_t delta = (int64_t)(uintptr_t)image_buffer - (int64_t)image_base;
    if (delta != 0 && opt_hdr_size >= 112 + 6 * 8) {
        uint32_t reloc_rva = *(const uint32_t *)(raw_file + opt_offset + 112 + 5 * 8);
        uint32_t reloc_size = *(const uint32_t *)(raw_file + opt_offset + 112 + 5 * 8 + 4);

        if (reloc_rva != 0 && reloc_size != 0 && (reloc_rva + reloc_size <= size_of_image)) {
            uint8_t *reloc_ptr = (uint8_t *)image_buffer + reloc_rva;
            uint8_t *reloc_end = reloc_ptr + reloc_size;

            while (reloc_ptr + 8 <= reloc_end) {
                uint32_t page_rva = *(uint32_t *)reloc_ptr;
                uint32_t block_sz = *(uint32_t *)(reloc_ptr + 4);
                if (block_sz < 8 || reloc_ptr + block_sz > reloc_end) break;

                uint16_t *entries = (uint16_t *)(reloc_ptr + 8);
                size_t count = (block_sz - 8) / 2;
                for (size_t j = 0; j < count; j++) {
                    uint16_t type = entries[j] >> 12;
                    uint16_t offset = entries[j] & 0x0FFF;
                    if (type == 10) { /* IMAGE_REL_BASED_DIR64 */
                        uint64_t *patch = (uint64_t *)((uint8_t *)image_buffer + page_rva + offset);
                        *patch += (uint64_t)delta;
                    } else if (type == 3) { /* IMAGE_REL_BASED_HIGHLOW */
                        uint32_t *patch = (uint32_t *)((uint8_t *)image_buffer + page_rva + offset);
                        *patch += (uint32_t)delta;
                    }
                }
                reloc_ptr += block_sz;
            }
        }
    }

    *out_image_base = image_buffer;
    *out_entry_point = (uint64_t)(uintptr_t)image_buffer + entry_rva;
    if (out_image_size) *out_image_size = size_of_image;

    klog_info("PE loaded: entry=0x%016llX, base=0x%016llX, delta=0x%llX",
              (unsigned long long)*out_entry_point, (unsigned long long)(uintptr_t)image_buffer, (unsigned long long)delta);
    return 0;
}

process_t *pe_spawn_process(const char *name, const char *path, process_privilege_t priv) {
    if (!path) return NULL;

    char norm_path[256];
    size_t j = 0;
    for (size_t i = 0; path[i] != '\0' && j < sizeof(norm_path) - 1; i++) {
        if (path[i] == '\\') {
            norm_path[j++] = '/';
        } else {
            norm_path[j++] = path[i];
        }
    }
    norm_path[j] = '\0';

    vfs_node_t *node = vfs_find_node(norm_path);
    if (!node || node->type != VFS_NODE_FILE || !node->content || node->size == 0) {
        klog_warn("PE spawn: file '%s' not found or empty in VFS", norm_path);
        return NULL;
    }

    void *image_base = NULL;
    uint64_t entry_point = 0;
    size_t image_size = 0;

    int res = pe_load_binary((const uint8_t *)node->content, node->size, &image_base, &entry_point, &image_size);
    if (res != 0 || entry_point == 0) {
        klog_err("PE spawn: failed to load PE binary from '%s' (err=%d)", norm_path, res);
        return NULL;
    }

    const char *proc_name = name;
    if (!proc_name) {
        proc_name = strrchr(norm_path, '/');
        if (proc_name) proc_name++;
        else proc_name = norm_path;
    }

    process_t *proc = process_create(proc_name, (void (*)(void))(uintptr_t)entry_point, priv);
    if (!proc) {
        klog_err("PE spawn: failed to create process for '%s'", proc_name);
        kfree(image_base);
        return NULL;
    }

    klog_info("PE spawn: launched PID %u '%s' from %s", proc->pid, proc->name, norm_path);
    return proc;
}
