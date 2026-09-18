#include "ipc.h"
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "lib.h"
#include "klog.h"
#include "drivers.h"
#include "pit.h"

#define IPC_RX_BUF_SIZE 8192
#define MAX_PENDING_CONNS 16

/* Standard socket states */
typedef enum {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_CREATED,
    SOCK_STATE_BOUND,
    SOCK_STATE_LISTENING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_DISCONNECTED
} ipc_sock_state_t;

typedef struct {
    int in_use;
    int domain;
    int type;
    int protocol;
    uint32_t owner_pid;
    char bind_path[UNIX_PATH_MAX];
    int is_listening;
    int backlog;
    ipc_sock_state_t state;
    int peer_idx; /* Index in g_sockets, or -1 */

    /* Pending accepted connections for listening socket */
    int pending_conns[MAX_PENDING_CONNS];
    int pending_head;
    int pending_tail;
    int pending_count;

    /* Receive buffer */
    uint8_t rx_buf[IPC_RX_BUF_SIZE];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
} ipc_socket_t;

static ipc_socket_t g_sockets[MAX_SOCKETS];

void ipc_init(void) {
    memset(g_sockets, 0, sizeof(g_sockets));
    for (int i = 0; i < MAX_SOCKETS; i++) {
        g_sockets[i].peer_idx = -1;
    }
    klog_info("IPC Unix-domain socket subsystem initialized");
}

static inline int fd_to_idx(int sockfd) {
    int idx = sockfd - SOCKET_FD_BASE;
    if (idx < 0 || idx >= MAX_SOCKETS) return -1;
    return idx;
}

static inline int idx_to_fd(int idx) {
    return idx + SOCKET_FD_BASE;
}

int sys_socket(int domain, int type, int protocol) {
    if (domain != AF_UNIX) return -EINVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i].in_use) {
            memset(&g_sockets[i], 0, sizeof(ipc_socket_t));
            g_sockets[i].in_use = 1;
            g_sockets[i].domain = domain;
            g_sockets[i].type = type;
            g_sockets[i].protocol = protocol;
            g_sockets[i].owner_pid = pid;
            g_sockets[i].state = SOCK_STATE_CREATED;
            g_sockets[i].peer_idx = -1;
            return idx_to_fd(i);
        }
    }
    return -ENOMEM;
}

int sys_bind(int sockfd, const sockaddr_un_t *addr, size_t addrlen) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;
    if (!addr || addrlen < sizeof(uint16_t)) return -EINVAL;
    if (addr->sun_family != AF_UNIX) return -EINVAL;

    ipc_socket_t *sock = &g_sockets[idx];
    if (sock->state != SOCK_STATE_CREATED) return -EINVAL;

    /* Check if path is already bound */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (g_sockets[i].in_use && g_sockets[i].state >= SOCK_STATE_BOUND) {
            if (strncmp(g_sockets[i].bind_path, addr->sun_path, UNIX_PATH_MAX) == 0) {
                return -EEXIST;
            }
        }
    }

    strncpy(sock->bind_path, addr->sun_path, UNIX_PATH_MAX - 1);
    sock->bind_path[UNIX_PATH_MAX - 1] = '\0';
    sock->state = SOCK_STATE_BOUND;
    return 0;
}

int sys_listen(int sockfd, int backlog) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;

    ipc_socket_t *sock = &g_sockets[idx];
    if (sock->state != SOCK_STATE_BOUND) return -EINVAL;

    sock->is_listening = 1;
    sock->backlog = (backlog > MAX_PENDING_CONNS || backlog <= 0) ? MAX_PENDING_CONNS : backlog;
    sock->state = SOCK_STATE_LISTENING;
    return 0;
}

int sys_accept(int sockfd, sockaddr_un_t *addr, size_t *addrlen) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;

    ipc_socket_t *sock = &g_sockets[idx];
    if (!sock->is_listening || sock->state != SOCK_STATE_LISTENING) return -EINVAL;

    process_t *curr = process_get_current();
    uint32_t pid = curr ? curr->pid : 0;

    /* If no connection is pending, yield or wait */
    uint64_t start_ticks = pit_get_ticks();
    while (sock->pending_count == 0) {
        scheduler_yield();
        if (pit_get_ticks() - start_ticks > 500) {
            return -EAGAIN;
        }
    }

    int conn_idx = sock->pending_conns[sock->pending_tail];
    sock->pending_tail = (sock->pending_tail + 1) % MAX_PENDING_CONNS;
    sock->pending_count--;

    ipc_socket_t *conn_sock = &g_sockets[conn_idx];
    conn_sock->owner_pid = pid;

    if (addr) {
        addr->sun_family = AF_UNIX;
        strncpy(addr->sun_path, sock->bind_path, UNIX_PATH_MAX - 1);
        addr->sun_path[UNIX_PATH_MAX - 1] = '\0';
        if (addrlen) *addrlen = sizeof(sockaddr_un_t);
    }

    return idx_to_fd(conn_idx);
}

int sys_connect(int sockfd, const sockaddr_un_t *addr, size_t addrlen) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;
    if (!addr || addrlen < sizeof(uint16_t)) return -EINVAL;
    if (addr->sun_family != AF_UNIX) return -EINVAL;

    ipc_socket_t *client_sock = &g_sockets[idx];
    if (client_sock->state == SOCK_STATE_CONNECTED) return -EISDIR;

    /* Find listening socket matching addr->sun_path */
    int srv_idx = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (g_sockets[i].in_use && g_sockets[i].is_listening) {
            if (strncmp(g_sockets[i].bind_path, addr->sun_path, UNIX_PATH_MAX) == 0) {
                srv_idx = i;
                break;
            }
        }
    }

    if (srv_idx < 0) return -ENOENT;
    ipc_socket_t *srv_sock = &g_sockets[srv_idx];

    if (srv_sock->pending_count >= srv_sock->backlog) {
        return -EAGAIN;
    }

    /* Allocate server-side connection socket */
    int conn_idx = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sockets[i].in_use) {
            conn_idx = i;
            break;
        }
    }
    if (conn_idx < 0) return -ENOMEM;

    ipc_socket_t *conn_sock = &g_sockets[conn_idx];
    memset(conn_sock, 0, sizeof(ipc_socket_t));
    conn_sock->in_use = 1;
    conn_sock->domain = client_sock->domain;
    conn_sock->type = client_sock->type;
    conn_sock->protocol = client_sock->protocol;
    conn_sock->owner_pid = srv_sock->owner_pid;
    conn_sock->state = SOCK_STATE_CONNECTED;
    conn_sock->peer_idx = idx;

    client_sock->peer_idx = conn_idx;
    client_sock->state = SOCK_STATE_CONNECTED;

    /* Enqueue to listener pending list */
    srv_sock->pending_conns[srv_sock->pending_head] = conn_idx;
    srv_sock->pending_head = (srv_sock->pending_head + 1) % MAX_PENDING_CONNS;
    srv_sock->pending_count++;

    return 0;
}

int64_t sys_send(int sockfd, const void *buf, size_t len, int flags) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;
    if (!buf && len > 0) return -EFAULT;
    if (len == 0) return 0;

    ipc_socket_t *sock = &g_sockets[idx];
    if (sock->state != SOCK_STATE_CONNECTED || sock->peer_idx < 0) {
        return -EBADF;
    }

    ipc_socket_t *peer = &g_sockets[sock->peer_idx];
    if (!peer->in_use || peer->state == SOCK_STATE_DISCONNECTED) {
        return -EIO;
    }

    size_t sent = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (sent < len) {
        size_t free_space = IPC_RX_BUF_SIZE - peer->rx_count;
        if (free_space == 0) {
            if (flags & MSG_DONTWAIT) {
                if (sent > 0) return (int64_t)sent;
                return -EAGAIN;
            }
            scheduler_yield();
            if (!peer->in_use || peer->state == SOCK_STATE_DISCONNECTED) {
                if (sent > 0) return (int64_t)sent;
                return -EIO;
            }
            continue;
        }

        size_t to_write = len - sent;
        if (to_write > free_space) to_write = free_space;

        for (size_t i = 0; i < to_write; i++) {
            peer->rx_buf[peer->rx_head] = src[sent++];
            peer->rx_head = (peer->rx_head + 1) % IPC_RX_BUF_SIZE;
        }
        peer->rx_count += to_write;
    }

    return (int64_t)sent;
}

int64_t sys_recv(int sockfd, void *buf, size_t len, int flags) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;
    if (!buf && len > 0) return -EFAULT;
    if (len == 0) return 0;

    ipc_socket_t *sock = &g_sockets[idx];
    if (sock->state != SOCK_STATE_CONNECTED && sock->state != SOCK_STATE_DISCONNECTED) {
        return -EBADF;
    }

    uint8_t *dst = (uint8_t *)buf;

    while (sock->rx_count == 0) {
        if (sock->state == SOCK_STATE_DISCONNECTED) {
            return 0; /* EOF */
        }
        if (flags & MSG_DONTWAIT) {
            return -EAGAIN;
        }
        scheduler_yield();
    }

    size_t to_read = len;
    if (to_read > sock->rx_count) to_read = sock->rx_count;

    for (size_t i = 0; i < to_read; i++) {
        dst[i] = sock->rx_buf[sock->rx_tail];
        sock->rx_tail = (sock->rx_tail + 1) % IPC_RX_BUF_SIZE;
    }
    sock->rx_count -= to_read;

    return (int64_t)to_read;
}

int sys_close_socket(int sockfd) {
    int idx = fd_to_idx(sockfd);
    if (idx < 0 || !g_sockets[idx].in_use) return -EBADF;

    ipc_socket_t *sock = &g_sockets[idx];

    /* Disconnect peer */
    if (sock->peer_idx >= 0 && sock->peer_idx < MAX_SOCKETS) {
        ipc_socket_t *peer = &g_sockets[sock->peer_idx];
        if (peer->in_use) {
            peer->state = SOCK_STATE_DISCONNECTED;
            peer->peer_idx = -1;
        }
    }

    sock->in_use = 0;
    sock->state = SOCK_STATE_FREE;
    sock->peer_idx = -1;
    sock->rx_count = 0;
    sock->pending_count = 0;

    return 0;
}

void ipc_close_process_sockets(uint32_t pid) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (g_sockets[i].in_use && g_sockets[i].owner_pid == pid) {
            sys_close_socket(idx_to_fd(i));
        }
    }
}

int sys_poll(pollfd_t *fds, size_t nfds, int timeout_ms) {
    if (!fds && nfds > 0) return -EFAULT;
    if (nfds == 0) return 0;

    uint64_t start_ticks = pit_get_ticks();
    uint64_t timeout_ticks = (timeout_ms > 0) ? (timeout_ms / 10 + 1) : 0;

    while (1) {
        int ready = 0;

        for (size_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;

            if (fd < 0) {
                continue;
            }

            /* Standard input (fd 0) */
            if (fd == 0) {
                extern int keyboard_has_char(void);
                if ((fds[i].events & POLLIN) && keyboard_has_char()) {
                    fds[i].revents |= POLLIN;
                }
            } else if (fd >= SOCKET_FD_BASE) {
                int idx = fd_to_idx(fd);
                if (idx < 0 || !g_sockets[idx].in_use) {
                    fds[i].revents |= POLLNVAL;
                } else {
                    ipc_socket_t *sock = &g_sockets[idx];

                    if (sock->is_listening) {
                        if ((fds[i].events & POLLIN) && sock->pending_count > 0) {
                            fds[i].revents |= POLLIN;
                        }
                    } else {
                        if (sock->state == SOCK_STATE_DISCONNECTED) {
                            fds[i].revents |= POLLHUP;
                            if (sock->rx_count > 0 && (fds[i].events & POLLIN)) {
                                fds[i].revents |= POLLIN;
                            }
                        } else if (sock->state == SOCK_STATE_CONNECTED) {
                            if ((fds[i].events & POLLIN) && sock->rx_count > 0) {
                                fds[i].revents |= POLLIN;
                            }
                            if (fds[i].events & POLLOUT) {
                                if (sock->peer_idx >= 0) {
                                    ipc_socket_t *peer = &g_sockets[sock->peer_idx];
                                    if (peer->rx_count < IPC_RX_BUF_SIZE) {
                                        fds[i].revents |= POLLOUT;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (fds[i].revents != 0) {
                ready++;
            }
        }

        if (ready > 0) {
            return ready;
        }

        if (timeout_ms == 0) {
            return 0; /* Non-blocking */
        }

        if (timeout_ms > 0 && (pit_get_ticks() - start_ticks) >= timeout_ticks) {
            return 0; /* Timeout reached */
        }

        scheduler_yield();
    }
}
