#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* System Call Numbers */
#define SYS_EXIT             1
#define SYS_EXEC             2
#define SYS_READ             3
#define SYS_WRITE            4
#define SYS_OPEN             5
#define SYS_CLOSE            6
#define SYS_WAITPID          7
#define SYS_GETPID           8
#define SYS_UNLINK           9
#define SYS_MKDIR            10
#define SYS_CHDIR            11
#define SYS_GETCWD           12
#define SYS_READDIR          13
#define SYS_YIELD            14
#define SYS_SLEEP            15

#define SYS_ELEVATE          20
#define SYS_DROP_PRIVILEGES  21
#define SYS_SUDO             22
#define SYS_GET_PRIVILEGE    23
#define SYS_TIME             24
#define SYS_DMESG            25
#define SYS_KILL             26
#define SYS_PS               27
#define SYS_REBOOT           28
#define SYS_SHUTDOWN         29
#define SYS_STAT             30
#define SYS_READFILE         31
#define SYS_WRITEFILE        32
#define SYS_GET_BOOTINFO     33
#define SYS_FLASH            34
#define SYS_FS               35
#define SYS_CPU              36
#define SYS_MEM              37
#define SYS_PCI              38
#define SYS_DEVPATH          39
#define SYS_ATTACHED_DRIVES  40
#define SYS_SWITCH_TARGET    41
#define SYS_SCREENRES        42
#define SYS_PROCTEST         43
#define SYS_HALT             44
#define SYS_CLEAR            45
#define SYS_GET_PROMPT_PATH  46
#define SYS_UPTIME           47
#define SYS_LISTDIR          48
#define SYS_GET_MOUSE_EVENT  50
#define SYS_GET_MOUSE_STATE  51
#define SYS_SOCKET           52
#define SYS_BIND             53
#define SYS_CONNECT          54
#define SYS_LISTEN           55
#define SYS_ACCEPT           56
#define SYS_SEND             57
#define SYS_RECV             58
#define SYS_POLL             59
#define SYS_SHM_CREATE       60
#define SYS_SHM_MAP          61
#define SYS_SHM_UNMAP        62
#define SYS_SHM_CLOSE        63
#define SYS_PANIC            99

/* VFS Node Types for SYS_STAT */
#define VFS_TYPE_FILE        0
#define VFS_TYPE_DIR         1

/* VFS Stat Structure for SYS_STAT */
typedef struct {
    uint32_t size;
    uint32_t type;
    int is_protected;
    char date_created[24];
    char date_accessed[24];
    char date_modified[24];
} vfs_stat_t;

/* Standard Error Numbers */
#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
#define ESRCH        3   /* No such process */
#define EIO          5   /* I/O error */
#define EBADF        9   /* Bad file number */
#define EAGAIN       11  /* Resource temporarily unavailable */
#define ENOMEM       12  /* Out of memory */
#define EACCES       13  /* Permission denied */
#define EFAULT       14  /* Bad address */
#define EEXIST       17  /* File exists */
#define ENODEV       19  /* No such device */
#define ENOTDIR      20  /* Not a directory */
#define EISDIR       21  /* Is a directory */
#define EINVAL       22  /* Invalid argument */
#define EPIPE        32  /* Broken pipe */
#define ENOSYS       38  /* Function not implemented */
#define EAFNOSUPPORT 97  /* Address family not supported */
#define EADDRINUSE   98  /* Address already in use */
#define EISCONN      106 /* Transport endpoint is already connected */
#define ENOTCONN     107 /* Transport endpoint is not connected */
#define ECONNREFUSED 111 /* Connection refused */

void syscall_init(void);
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

extern uint64_t g_current_process_privilege;

/* User & Freestanding Syscall Wrapper */
static inline int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Backward compatibility alias */
static inline int64_t pseu_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return syscall(num, a1, a2, a3, a4, a5);
}

#endif /* SYSCALL_H */
