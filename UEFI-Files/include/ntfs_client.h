#ifndef NTFS_CLIENT_H
#define NTFS_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
#include "ipc.h"
#include "shm.h"
#include "ntfs_protocol.h"

typedef struct {
    int sock;
    uint32_t window_id;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    int shm_id;
} ntfs_client_t;

static inline size_t ntfs_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static inline void *ntfs_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static inline void *ntfs_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static inline char *ntfs_strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

static inline int ntfs_client_connect(ntfs_client_t *client, const char *title, uint32_t width, uint32_t height, uint32_t flags) {
    if (!client) return -1;
    ntfs_memset(client, 0, sizeof(*client));
    client->sock = -1;
    client->shm_id = -1;

    int sock = (int)syscall(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0);
    if (sock < 0) return -1;

    sockaddr_un_t addr;
    ntfs_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    ntfs_strncpy(addr.sun_path, NTFS_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int connected = 0;
    for (int retry = 0; retry < 20; retry++) {
        if (syscall(SYS_CONNECT, sock, (uint64_t)(uintptr_t)&addr, sizeof(addr), 0, 0) == 0) {
            connected = 1;
            break;
        }
        syscall(SYS_SLEEP, 50, 0, 0, 0, 0);
    }
    if (!connected) {
        syscall(SYS_CLOSE, sock, 0, 0, 0, 0);
        return -1;
    }

    client->sock = sock;

    ntfs_msg_t cmsg;
    ntfs_memset(&cmsg, 0, sizeof(cmsg));
    cmsg.type = NTFS_MSG_CREATE_WINDOW;
    cmsg.width = width;
    cmsg.height = height;
    cmsg.flags = flags;
    if (title) ntfs_strncpy(cmsg.title, title, sizeof(cmsg.title) - 1);

    if (syscall(SYS_SEND, sock, (uint64_t)(uintptr_t)&cmsg, sizeof(cmsg), 0, 0) != (int64_t)sizeof(cmsg)) {
        syscall(SYS_CLOSE, sock, 0, 0, 0, 0);
        return -1;
    }

    ntfs_msg_t rep;
    if (syscall(SYS_RECV, sock, (uint64_t)(uintptr_t)&rep, sizeof(rep), 0, 0) != (int64_t)sizeof(rep) ||
        rep.type != NTFS_MSG_WINDOW_CREATED) {
        syscall(SYS_CLOSE, sock, 0, 0, 0, 0);
        return -1;
    }

    client->window_id = rep.window_id;
    client->width = rep.width;
    client->height = rep.height;
    client->shm_id = (int)rep.shm_id;

    if (client->shm_id > 0) {
        client->pixels = (uint32_t *)(uintptr_t)syscall(SYS_SHM_MAP, client->shm_id, 0, SHM_READ | SHM_WRITE, 0, 0);
    }

    return 0;
}

static inline void ntfs_client_damage(ntfs_client_t *client, int x, int y, int w, int h) {
    if (!client || client->sock < 0) return;
    ntfs_msg_t dmsg;
    ntfs_memset(&dmsg, 0, sizeof(dmsg));
    dmsg.type = NTFS_MSG_DAMAGE;
    dmsg.window_id = client->window_id;
    dmsg.x = x;
    dmsg.y = y;
    dmsg.width = (uint32_t)w;
    dmsg.height = (uint32_t)h;
    syscall(SYS_SEND, client->sock, (uint64_t)(uintptr_t)&dmsg, sizeof(dmsg), 0, 0);
}

static inline int ntfs_client_poll_event(ntfs_client_t *client, ntfs_msg_t *msg_out) {
    if (!client || client->sock < 0 || !msg_out) return 0;
    int64_t n = syscall(SYS_RECV, client->sock, (uint64_t)(uintptr_t)msg_out, sizeof(*msg_out), MSG_DONTWAIT, 0);
    if (n == 0 || (n < 0 && n != -EAGAIN)) {
        client->sock = -1;
        return 0;
    }
    if (n == (int64_t)sizeof(*msg_out)) {
        if (msg_out->type == NTFS_MSG_WINDOW_RESIZED) {
            client->width = msg_out->width;
            client->height = msg_out->height;
        }
        return 1;
    }
    return 0;
}

static inline void ntfs_client_close(ntfs_client_t *client) {
    if (!client) return;
    if (client->pixels && client->shm_id > 0) {
        syscall(SYS_SHM_UNMAP, (uint64_t)(uintptr_t)client->pixels, 0, 0, 0, 0);
        client->pixels = NULL;
    }
    if (client->sock >= 0) {
        ntfs_msg_t dmsg;
        ntfs_memset(&dmsg, 0, sizeof(dmsg));
        dmsg.type = NTFS_MSG_DESTROY_WINDOW;
        dmsg.window_id = client->window_id;
        syscall(SYS_SEND, client->sock, (uint64_t)(uintptr_t)&dmsg, sizeof(dmsg), 0, 0);
        syscall(SYS_CLOSE, client->sock, 0, 0, 0, 0);
        client->sock = -1;
    }
}

#endif /* NTFS_CLIENT_H */
