#ifndef PE_LOADER_H
#define PE_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"

/* Load PE32+ binary into execution memory and resolve base relocations */
int pe_load_binary(const uint8_t *raw_file, size_t raw_size, void **out_image_base, uint64_t *out_entry_point, size_t *out_image_size);

/* Load PE32+ executable from VFS and create a runnable process */
process_t *pe_spawn_process(const char *name, const char *path, process_privilege_t priv);

#endif /* PE_LOADER_H */
