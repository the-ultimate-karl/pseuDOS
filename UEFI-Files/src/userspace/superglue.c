#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

void superglue_main(void) {
    /* Backwards compatibility shim: execute /protected/gui/gman.exe */
    syscall(SYS_EXEC, (uint64_t)(uintptr_t)"/protected/gui/gman.exe", 1, 0, 0, 0);
}
