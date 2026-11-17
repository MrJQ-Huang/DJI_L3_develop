/* SDK-free relay: bounded Annex-B H.264 on stdin -> existing TCP distributor.
 * Intended for an onboard encoder, never for point packets or recordings. */
#define _POSIX_C_SOURCE 200809L
#include "l3_stream.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) { (void)signal_number; stopped = 1; }
static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("Usage: l3_video_relay VIDEO_PORT SECONDS (1..300), H.264 on stdin\n", stderr);
        return 2;
    }
    char *end;
    unsigned long port = strtoul(argv[1], &end, 10);
    if (!*argv[1] || *end || port > 65535) return 2;
    unsigned long seconds = strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || seconds < 1 || seconds > 300) return 2;
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    L3StreamConfig config = {.bind_ip = "127.0.0.1", .video_port = (uint16_t)port};
    L3Stream *stream = l3_stream_start(&config);
    if (!stream) { perror("video relay"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    L3StreamStats stats;
    l3_stream_stats(stream, &stats);
    printf("{\"event\":\"video_relay_ready\",\"video_port\":%u}\n", stats.video_port);
    int64_t begin = milliseconds();
    int result = begin < 0 ? 1 : 0;
    uint8_t buffer[65536];
    while (!result && !stopped) {
        int64_t now = milliseconds();
        if (now < 0) { result = 1; break; }
        if (now - begin >= (int64_t)seconds * 1000) break;
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = poll(&input, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            result = 1; break;
        }
        if (!ready) continue;
        if (input.revents & (POLLERR | POLLNVAL)) { result = 1; break; }
        ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length == 0) break;
        if (length < 0) {
            if (errno == EINTR) continue;
            result = 1; break;
        }
        if (!l3_stream_publish_h264(stream, buffer, (size_t)length)) result = 1;
    }
    l3_stream_stats(stream, &stats);
    printf("{\"event\":\"video_relay_complete\",\"video_bytes\":%" PRIu64
           ",\"connections\":%" PRIu64 ",\"slow_disconnects\":%" PRIu64
           ",\"send_errors\":%" PRIu64 ",\"queue_high_water\":%zu}\n",
           stats.video_bytes, stats.connections, stats.slow_disconnects,
           stats.send_errors, stats.video_queue_high_water);
    l3_stream_stop(stream);
    return result || stats.video_bytes == 0 ? 1 : 0;
}
