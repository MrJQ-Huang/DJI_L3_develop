#define _POSIX_C_SOURCE 200809L
#include "l3_stream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void pause_ms(long ms)
{
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

static int connect_to(uint16_t port, bool small_receive_buffer)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (small_receive_buffer) {
        int size = 4096;
        assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
    }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port) };
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static void wait_clients(L3Stream *stream, unsigned points, unsigned video)
{
    for (unsigned i = 0; i < 400; ++i) {
        L3StreamStats stats;
        l3_stream_stats(stream, &stats);
        if (stats.point_clients == points && stats.video_clients == video) return;
        pause_ms(5);
    }
    assert(!"client acceptance did not finish");
}

static void read_exact(int fd, uint8_t *out, size_t size)
{
    size_t used = 0;
    while (used < size) {
        struct pollfd wait = { .fd = fd, .events = POLLIN };
        assert(poll(&wait, 1, 2000) > 0);
        ssize_t count = recv(fd, out + used, size - used, 0);
        assert(count > 0);
        used += (size_t)count;
    }
}

static void write_le(uint8_t *p, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void point_packet(uint8_t *out, size_t size, uint32_t sequence)
{
    assert(size >= 44 && (size - 28) % 16 == 0);
    memset(out, 0, size);
    write_le(out, UINT32_MAX);
    write_le(out + 4, sequence);
    write_le(out + 16, (uint32_t)(size - 28));
}

/* Each call supplies a test NAL and a following delimiter. Small NALs are
 * published byte-by-byte, including start-code and NAL-header splits. */
static void nal(L3Stream *stream, uint8_t type, uint8_t first, size_t payload)
{
    size_t size = payload + 5;
    uint8_t *data = malloc(size + 4);
    assert(data != NULL && payload > 0);
    memcpy(data, "\0\0\0\1", 4);
    data[4] = type;
    memset(data + 5, 0x55, payload);
    data[5] = first;
    memcpy(data + size, "\0\0\0\1", 4);
    if (size < 32) {
        for (size_t i = 0; i < size + 4; ++i)
            assert(l3_stream_publish_h264(stream, data + i, 1));
    } else assert(l3_stream_publish_h264(stream, data, size + 4));
    free(data);
}

static void bootstrap(L3Stream *stream)
{
    nal(stream, 0x67, 0x42, 2);
    nal(stream, 0x68, 0xce, 1);
    nal(stream, 0x65, 0x80, 2);
}

static void test_dual_stream_and_join(void)
{
    L3StreamConfig config = {0};
    L3Stream *stream = l3_stream_start(&config);
    assert(stream != NULL);
    L3StreamStats stats;
    l3_stream_stats(stream, &stats);
    int points[2] = { connect_to(stats.point_port, false), connect_to(stats.point_port, false) };
    int videos[2] = { connect_to(stats.video_port, false), connect_to(stats.video_port, false) };
    wait_clients(stream, 2, 2);

    uint8_t packet[44], framed[52];
    point_packet(packet, sizeof(packet), 7);
    assert(l3_stream_publish_points(stream, packet, sizeof(packet)));
    for (size_t i = 0; i < 2; ++i) {
        read_exact(points[i], framed, sizeof(framed));
        assert(memcmp(framed, "L3P1\0\0\0\54", 8) == 0);
        assert(memcmp(framed + 8, packet, sizeof(packet)) == 0);
    }

    nal(stream, 0x67, 0x42, 2);
    nal(stream, 0x68, 0xce, 1);
    nal(stream, 0x65, 0x40, 2); /* first_mb_in_slice=1: cannot join here. */
    struct pollfd empty = { .fd = videos[0], .events = POLLIN };
    assert(poll(&empty, 1, 40) == 0);
    nal(stream, 0x65, 0x80, 2);
    const uint8_t expected[] = {0,0,0,1,0x67,0x42,0x55, 0,0,0,1,0x68,0xce,
                                0,0,0,1,0x65,0x80,0x55};
    uint8_t received[sizeof(expected)];
    for (size_t i = 0; i < 2; ++i) {
        read_exact(videos[i], received, sizeof(received));
        assert(memcmp(received, expected, sizeof(expected)) == 0);
    }

    int excess = connect_to(stats.point_port, false);
    struct pollfd rejected = { .fd = excess, .events = POLLIN };
    assert(poll(&rejected, 1, 2000) > 0);
    uint8_t byte;
    assert(recv(excess, &byte, 1, 0) == 0);
    close(excess);
    close(videos[0]); close(videos[1]);
    for (size_t i = 0; i < 6; ++i) { nal(stream, 0x41, 0x80, 4); pause_ms(10); }
    wait_clients(stream, 2, 0);
    int joined = connect_to(stats.video_port, false);
    wait_clients(stream, 2, 1);
    nal(stream, 0x41, 0x80, 4);
    empty.fd = joined;
    assert(poll(&empty, 1, 40) == 0);
    nal(stream, 0x65, 0x80, 2);
    read_exact(joined, received, sizeof(received));
    assert(memcmp(received, expected, sizeof(expected)) == 0);
    close(joined); close(points[0]); close(points[1]);
    wait_clients(stream, 0, 0); /* Idle closed clients free their slots too. */
    assert(!l3_stream_publish_points(stream, packet, sizeof(packet) - 1));
    assert(!l3_stream_publish_h264(stream, NULL, 0));
    l3_stream_stop(stream);
    puts("PASS: two clients per stream, point framing, split NALs, IDR join, reconnect, client cap");
}

static void test_slow_video_client(void)
{
    L3StreamConfig config = { .video_queue_bytes = 8192, .send_timeout_ms = 250 };
    L3Stream *stream = l3_stream_start(&config);
    assert(stream != NULL);
    L3StreamStats stats;
    l3_stream_stats(stream, &stats);
    int slow = connect_to(stats.video_port, true), fast = connect_to(stats.video_port, false);
    wait_clients(stream, 0, 2);
    bootstrap(stream);
    uint8_t data[4096];
    read_exact(fast, data, 20);
    for (size_t i = 0; i < 80; ++i) {
        nal(stream, 0x41, 0x80, sizeof(data) - 5);
        read_exact(fast, data, sizeof(data));
    }
    l3_stream_stats(stream, &stats);
    assert(stats.slow_disconnects >= 1 && stats.video_clients == 1);
    assert(stats.video_queue_high_water <= config.video_queue_bytes);
    close(slow); close(fast);
    l3_stream_stop(stream);
    puts("PASS: slow video client disconnected while fast client remains live, bounded memory");
}

static void test_point_drop_and_stop(void)
{
    /* A record is 2052 bytes. Permit an in-flight record plus the next one:
     * the receiver can finish recv before the writer updates its accounting.
     * A 4096-byte cap would legitimately drop the next fast-peer record on
     * that scheduling order, rather than testing only the stalled peer. */
    L3StreamConfig config = { .point_queue_bytes = 8192, .send_timeout_ms = 250 };
    L3Stream *stream = l3_stream_start(&config);
    assert(stream != NULL);
    L3StreamStats stats;
    l3_stream_stats(stream, &stats);
    int slow = connect_to(stats.point_port, true), fast = connect_to(stats.point_port, false);
    wait_clients(stream, 2, 0);
    uint8_t packet[2044], framed[2052];
    for (size_t i = 0; i < 100; ++i) {
        point_packet(packet, sizeof(packet), (uint32_t)i);
        assert(l3_stream_publish_points(stream, packet, sizeof(packet)));
        read_exact(fast, framed, sizeof(framed));
        assert(memcmp(framed, "L3P1", 4) == 0);
        assert(memcmp(framed + 8, packet, sizeof(packet)) == 0);
    }
    l3_stream_stats(stream, &stats);
    assert(stats.point_dropped > 0);
    assert(stats.point_queue_high_water <= config.point_queue_bytes);
    l3_stream_stop(stream);
    close(slow); close(fast);
    puts("PASS: drop only whole queued point records; fast peer unaffected; stop with blocked peer");
}

static void test_limits_and_lifecycle(void)
{
    L3StreamConfig config = { .bind_ip = "invalid" };
    assert(l3_stream_start(&config) == NULL && errno == EINVAL);
    config.bind_ip = NULL;
    for (size_t iteration = 0; iteration < 5; ++iteration) {
        L3Stream *stream = l3_stream_start(&config);
        assert(stream != NULL);
        uint8_t *oversized = malloc(1024 * 1024 + 16);
        assert(oversized != NULL);
        memcpy(oversized, "\0\0\0\1\x65\x80", 6);
        memset(oversized + 6, 0x55, 1024 * 1024 + 10);
        assert(!l3_stream_publish_h264(stream, oversized, 1024 * 1024 + 16));
        assert(!l3_stream_publish_h264(stream, (const uint8_t *)"x", 1));
        free(oversized);
        l3_stream_stop(stream);
    }
    puts("PASS: invalid bind, oversized NAL rejection, sticky stream failure, repeated cleanup");
}

int main(void)
{
    test_dual_stream_and_join();
    test_slow_video_client();
    test_point_drop_and_stop();
    test_limits_and_lifecycle();
    return 0;
}
