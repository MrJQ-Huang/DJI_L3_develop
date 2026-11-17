#ifndef L3_STREAM_H
#define L3_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct L3Stream L3Stream;

typedef struct {
    const char *bind_ip; /* Explicit numeric IPv4; NULL means 127.0.0.1. */
    uint16_t point_port; /* 0 asks the OS for a free port. */
    uint16_t video_port;
    size_t point_queue_bytes; /* 0 = 1 MiB per client. */
    size_t video_queue_bytes; /* 0 = 2 MiB per client. */
    uint32_t send_timeout_ms; /* 0 = 1000 ms per record. */
} L3StreamConfig;

typedef struct {
    uint16_t point_port;
    uint16_t video_port;
    uint32_t point_clients;
    uint32_t video_clients;
    uint64_t point_packets;
    uint64_t video_bytes;
    uint64_t point_dropped;
    uint64_t slow_disconnects;
    uint64_t send_errors;
    uint64_t connections;
    size_t point_queue_high_water;
    size_t video_queue_high_water;
} L3StreamStats;

/* SDK-independent TCP distributor: L3P1 packets and raw Annex-B H.264.
 * At most two clients per stream. Start/stop never call the DJI SDK.
 * Start returns NULL and sets errno on failure. Publish copies borrowed bytes
 * and does no socket I/O. It may be called from independent source threads.
 * Video joins wait for cached SPS/PPS and the first slice of a complete IDR;
 * callback/NAL boundaries are not assumed to coincide.
 */
L3Stream *l3_stream_start(const L3StreamConfig *config);
bool l3_stream_publish_h264(L3Stream *stream, const uint8_t *data, size_t size);
bool l3_stream_publish_points(L3Stream *stream, const uint8_t *packet, size_t size);
void l3_stream_stats(L3Stream *stream, L3StreamStats *stats);

/* Stop source callbacks before stop. Stop closes connections and joins all
 * workers; the stream pointer is invalid afterwards. */
void l3_stream_stop(L3Stream *stream);

#ifdef __cplusplus
}
#endif
#endif
