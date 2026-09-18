#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>

#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define MSG_DONTWAIT    0x40

#define POLLIN          0x0001
#define POLLPRI         0x0002
#define POLLOUT         0x0004
#define POLLERR         0x0008
#define POLLHUP         0x0010
#define POLLNVAL        0x0020

typedef struct {
    int fd;
    short events;
    short revents;
} pollfd_t;

#define UNIX_PATH_MAX 108

typedef struct {
    uint16_t sun_family;
    char sun_path[UNIX_PATH_MAX];
} sockaddr_un_t;

#define SOCKET_FD_BASE 10
#define MAX_SOCKETS    64

void ipc_init(void);
int sys_socket(int domain, int type, int protocol);
int sys_bind(int sockfd, const sockaddr_un_t *addr, size_t addrlen);
int sys_listen(int sockfd, int backlog);
int sys_accept(int sockfd, sockaddr_un_t *addr, size_t *addrlen);
int sys_connect(int sockfd, const sockaddr_un_t *addr, size_t addrlen);
int64_t sys_send(int sockfd, const void *buf, size_t len, int flags);
int64_t sys_recv(int sockfd, void *buf, size_t len, int flags);
int sys_close_socket(int sockfd);
int sys_poll(pollfd_t *fds, size_t nfds, int timeout_ms);
void ipc_close_process_sockets(uint32_t pid);

#endif /* IPC_H */
