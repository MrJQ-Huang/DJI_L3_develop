#define _POSIX_C_SOURCE 200809L
#include "metadata_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dji_error.h"

enum { IO_WAIT_MS = 250, CONNECT_WAIT_MS = 1500, SOCKET_BUFFER_BYTES = 256 * 1024 };

typedef struct {
    int fd;
    E_DjiSocketMode mode;
} MetadataSocket;

static T_DjiReturnCode socket_error(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT || error == EINTR
               ? DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT
               : DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
}

static bool address_of(const char *ip, uint32_t port, struct sockaddr_in *address)
{
    if (ip == NULL || port > UINT16_MAX) {
        return false;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &address->sin_addr) == 1;
}

static bool configure_socket(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);
    int reuse = 1;
    int size = SOCKET_BUFFER_BYTES;
    return flags >= 0 && descriptor_flags >= 0 &&
           fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0;
}

/* Recompute the remaining budget after a signal; repeated EINTR cannot extend
 * the deadline. The actual socket syscall is nonblocking if readiness races. */
static T_DjiReturnCode wait_socket(int fd, short events, int timeout_ms)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    int remaining = timeout_ms;
    for (;;) {
        struct pollfd item = { .fd = fd, .events = events, .revents = 0 };
        int result = poll(&item, 1, remaining);
        if (result > 0) {
            if (item.revents & POLLNVAL) {
                return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
            }
            /* Read on POLLHUP to report EOF; connect checks SO_ERROR. */
            return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
        }
        if (result == 0) {
            return DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
        }
        if (errno != EINTR || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
        }
        int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= timeout_ms) {
            return DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
        }
        remaining = timeout_ms - (int)elapsed;
    }
}

static T_DjiReturnCode create_socket(E_DjiSocketMode mode, T_DjiSocketHandle *out)
{
    if (out == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    *out = NULL;
    if (mode != DJI_SOCKET_MODE_UDP && mode != DJI_SOCKET_MODE_TCP) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    int fd = socket(AF_INET, mode == DJI_SOCKET_MODE_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        return socket_error(errno);
    }
    if (!configure_socket(fd)) {
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    MetadataSocket *handle = malloc(sizeof(*handle));
    if (handle == NULL) {
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_MEMORY_ALLOC_FAILED;
    }
    handle->fd = fd;
    handle->mode = mode;
    *out = handle;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode close_socket(T_DjiSocketHandle raw)
{
    MetadataSocket *handle = raw;
    if (handle == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    /* Do not retry close on EINTR: Linux already releases the descriptor. */
    int result = close(handle->fd);
    int error = errno;
    free(handle);
    return result == 0 ? DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS : socket_error(error);
}

static T_DjiReturnCode bind_socket(T_DjiSocketHandle raw, const char *ip, uint32_t port)
{
    MetadataSocket *handle = raw;
    struct sockaddr_in address;
    if (handle == NULL || !address_of(ip, port, &address)) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    return bind(handle->fd, (struct sockaddr *)&address, sizeof(address)) == 0
               ? DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS : socket_error(errno);
}

static T_DjiReturnCode udp_send(T_DjiSocketHandle raw, const char *ip, uint32_t port,
                               const uint8_t *data, uint32_t size, uint32_t *sent)
{
    if (sent != NULL) {
        *sent = 0;
    }
    MetadataSocket *handle = raw;
    struct sockaddr_in address;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_UDP || sent == NULL ||
        data == NULL || size == 0 || port == 0 || !address_of(ip, port, &address)) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLOUT, IO_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    ssize_t count = sendto(handle->fd, data, size, MSG_NOSIGNAL,
                           (struct sockaddr *)&address, sizeof(address));
    if (count < 0) {
        return socket_error(errno);
    }
    *sent = (uint32_t)count;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode udp_receive(T_DjiSocketHandle raw, char *ip, uint32_t *port,
                                  uint8_t *data, uint32_t size, uint32_t *received)
{
    if (received != NULL) {
        *received = 0;
    }
    MetadataSocket *handle = raw;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_UDP || ip == NULL || port == NULL ||
        data == NULL || size == 0 || received == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    ip[0] = '\0';
    *port = 0;
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLIN, IO_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    /* Linux returns the full datagram size with MSG_TRUNC. Reject truncation
     * instead of claiming that a partial control packet was complete. */
    ssize_t count = recvfrom(handle->fd, data, size, MSG_TRUNC,
                             (struct sockaddr *)&address, &length);
    if (count < 0) {
        return socket_error(errno);
    }
    if ((uint64_t)count > size) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_OUT_OF_RANGE;
    }
    if (length != sizeof(address) || address.sin_family != AF_INET ||
        inet_ntop(AF_INET, &address.sin_addr, ip, INET_ADDRSTRLEN) == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    *port = ntohs(address.sin_port);
    *received = (uint32_t)count;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode tcp_listen(T_DjiSocketHandle raw)
{
    MetadataSocket *handle = raw;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_TCP) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    return listen(handle->fd, 4) == 0 ? DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS : socket_error(errno);
}

static T_DjiReturnCode tcp_accept(T_DjiSocketHandle raw, char *ip, uint32_t *port,
                                 T_DjiSocketHandle *out)
{
    if (out != NULL) {
        *out = NULL;
    }
    MetadataSocket *handle = raw;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_TCP || ip == NULL || port == NULL || out == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    ip[0] = '\0';
    *port = 0;
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLIN, IO_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = accept(handle->fd, (struct sockaddr *)&address, &length);
    if (fd < 0) {
        return socket_error(errno);
    }
    if (length != sizeof(address) || address.sin_family != AF_INET || !configure_socket(fd) ||
        inet_ntop(AF_INET, &address.sin_addr, ip, INET_ADDRSTRLEN) == NULL) {
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    MetadataSocket *accepted = malloc(sizeof(*accepted));
    if (accepted == NULL) {
        close(fd);
        return DJI_ERROR_SYSTEM_MODULE_CODE_MEMORY_ALLOC_FAILED;
    }
    accepted->fd = fd;
    accepted->mode = DJI_SOCKET_MODE_TCP;
    *port = ntohs(address.sin_port);
    *out = accepted;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode tcp_connect(T_DjiSocketHandle raw, const char *ip, uint32_t port)
{
    MetadataSocket *handle = raw;
    struct sockaddr_in address;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_TCP || port == 0 ||
        !address_of(ip, port, &address)) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    if (connect(handle->fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    }
    if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
        return socket_error(errno);
    }
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLOUT, CONNECT_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(handle->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return socket_error(errno);
    }
    return error == 0 ? DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS : socket_error(error);
}

static T_DjiReturnCode tcp_send(T_DjiSocketHandle raw, const uint8_t *data,
                               uint32_t size, uint32_t *sent)
{
    if (sent != NULL) {
        *sent = 0;
    }
    MetadataSocket *handle = raw;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_TCP || data == NULL || size == 0 || sent == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLOUT, IO_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    ssize_t count = send(handle->fd, data, size, MSG_NOSIGNAL);
    if (count < 0) {
        return socket_error(errno);
    }
    *sent = (uint32_t)count;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static T_DjiReturnCode tcp_receive(T_DjiSocketHandle raw, uint8_t *data,
                                  uint32_t size, uint32_t *received)
{
    if (received != NULL) {
        *received = 0;
    }
    MetadataSocket *handle = raw;
    if (handle == NULL || handle->mode != DJI_SOCKET_MODE_TCP || data == NULL || size == 0 || received == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    T_DjiReturnCode rc = wait_socket(handle->fd, POLLIN, IO_WAIT_MS);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        return rc;
    }
    ssize_t count = recv(handle->fd, data, size, 0);
    if (count < 0) {
        return socket_error(errno);
    }
    *received = (uint32_t)count; /* Zero is an orderly TCP EOF, not a packet. */
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

const T_DjiSocketHandler *L3Metadata_GetSocketHandler(void)
{
    static const T_DjiSocketHandler handler = {
        .Socket = create_socket,
        .Close = close_socket,
        .Bind = bind_socket,
        .UdpSendData = udp_send,
        .UdpRecvData = udp_receive,
        .TcpListen = tcp_listen,
        .TcpAccept = tcp_accept,
        .TcpConnect = tcp_connect,
        .TcpSendData = tcp_send,
        .TcpRecvData = tcp_receive,
    };
    return &handler;
}
