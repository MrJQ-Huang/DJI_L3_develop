#define _POSIX_C_SOURCE 200809L
#include "l3p1_tcp_sink.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

static int64_t monotonic_ms(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
        return -1;
    }
    return (int64_t)stamp.tv_sec * 1000 + stamp.tv_nsec / 1000000;
}

static int send_until(int fd, const uint8_t *data, size_t size, int64_t deadline)
{
    while (size > 0) {
        int64_t now = monotonic_ms();
        if (now < 0) return errno;
        if (now >= deadline) return ETIMEDOUT;

        ssize_t sent = send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            size -= (size_t)sent;
            continue;
        }
        if (sent == 0) return EPIPE;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return errno;

        struct pollfd wait = { .fd = fd, .events = POLLOUT };
        int ready = poll(&wait, 1, (int)(deadline - now));
        if (ready == 0) return ETIMEDOUT;
        if (ready < 0 && errno != EINTR) return errno;
        if (ready > 0 && (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) return EPIPE;
    }
    return 0;
}

bool l3p1_tcp_sink(const uint8_t *packet, uint32_t packet_size, void *context)
{
    L3p1TcpSink *sink = context;
    if (sink == NULL || sink->failed) return false;
    if (sink->fd < 0 || sink->timeout_ms == 0 || sink->timeout_ms > 60000 ||
        packet == NULL || packet_size < 44 || packet_size > 512U * 1024U) {
        sink->error_number = EINVAL;
        sink->failed = true;
        return false;
    }
    int64_t now = monotonic_ms();
    if (now < 0) {
        sink->error_number = errno;
        sink->failed = true;
        return false;
    }
    int64_t deadline = now + sink->timeout_ms;
    uint8_t header[] = {
        'L', '3', 'P', '1', (uint8_t)(packet_size >> 24), (uint8_t)(packet_size >> 16),
        (uint8_t)(packet_size >> 8), (uint8_t)packet_size
    };
    int error = send_until(sink->fd, header, sizeof(header), deadline);
    if (error == 0) error = send_until(sink->fd, packet, packet_size, deadline);
    if (error == 0) return true;

    /* Never append a new record after a partial write on the same stream. */
    sink->error_number = error;
    sink->failed = true;
    (void)shutdown(sink->fd, SHUT_WR);
    return false;
}
