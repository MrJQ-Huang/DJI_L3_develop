/* Offline integration fixture: loop a locally generated Annex-B H.264 file
 * plus synthetic point records. It never links or calls DJI PSDK. */
#define _POSIX_C_SOURCE 200809L
#include "l3_stream.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t interrupted;

static void request_stop(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static void put_float(uint8_t *p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(p, bits);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 5 || argc == 4) {
        fputs("Usage: stream_replay GENERATED.h264 [SECONDS=5 [POINT_PORT VIDEO_PORT]]\n", stderr);
        return 2;
    }
    char *end = NULL;
    unsigned long seconds = argc >= 3 ? strtoul(argv[2], &end, 10) : 5;
    if (seconds == 0 || seconds > 60 || (argc >= 3 && (end == argv[2] || *end != '\0'))) return 2;
    unsigned long ports[2] = {0, 0};
    if (argc == 5) {
        for (size_t i = 0; i < 2; ++i) {
            ports[i] = strtoul(argv[3 + i], &end, 10);
            if (ports[i] > 65535 || end == argv[3 + i] || *end != '\0') return 2;
        }
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) { perror("H.264 fixture"); return 1; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return 1;
    }
    uint8_t *video = malloc((size_t)length);
    if (video == NULL || fread(video, 1, (size_t)length, file) != (size_t)length) {
        free(video); fclose(file); return 1;
    }
    fclose(file);
    struct sigaction action = { .sa_handler = request_stop };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    L3StreamConfig config = {0};
    config.point_port = (uint16_t)ports[0];
    config.video_port = (uint16_t)ports[1];
    L3Stream *stream = l3_stream_start(&config);
    if (stream == NULL) { perror("stream start"); free(video); return 1; }
    L3StreamStats stats;
    l3_stream_stats(stream, &stats);
    printf("{\"event\":\"synthetic_replay\",\"point_port\":%u,\"video_port\":%u}\n",
           stats.point_port, stats.video_port);
    fflush(stdout);
    int status = 0;
    for (uint32_t sequence = 0; sequence < seconds * 10 && !interrupted; ++sequence) {
        for (size_t offset = 0; offset < (size_t)length; offset += 137) {
            size_t size = (size_t)length - offset;
            if (size > 137) size = 137;
            if (!l3_stream_publish_h264(stream, video + offset, size)) { status = 1; break; }
        }
        if (status != 0) break;
        uint8_t point[44] = {0};
        put_u32(point, UINT32_MAX);
        put_u32(point + 4, sequence);
        put_u32(point + 8, sequence * 1000);
        put_u32(point + 16, 16);
        put_float(point + 28, 1.25f);
        put_float(point + 32, -2.5f);
        put_float(point + 36, 3.5f);
        point[40] = 95; point[41] = 20; point[42] = 60; point[43] = 110;
        if (!l3_stream_publish_points(stream, point, sizeof(point))) { status = 1; break; }
        struct timespec wait = { .tv_nsec = 100 * 1000 * 1000 };
        while (nanosleep(&wait, &wait) != 0 && errno == EINTR && !interrupted) {}
    }
    l3_stream_stats(stream, &stats);
    printf("{\"event\":\"replay_stopped\",\"point_packets\":%llu,\"video_bytes\":%llu}\n",
           (unsigned long long)stats.point_packets, (unsigned long long)stats.video_bytes);
    l3_stream_stop(stream);
    free(video);
    return status;
}
