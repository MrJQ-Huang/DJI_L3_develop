#ifndef L3P1_TCP_SINK_H
#define L3P1_TCP_SINK_H

#include <stdbool.h>
#include <stdint.h>

/* Linux/Manifold 3 only. The caller opens and closes the connected TCP socket.
 * One writer per socket. timeout_ms limits the complete envelope and packet.
 * Initialize failed=false, error_number=0; a failed stream must be replaced.
 */
typedef struct {
    int fd;
    uint32_t timeout_ms;
    bool failed;
    int error_number;
} L3p1TcpSink;

/* Compatible with L3PointCloudPacketSink. No connection or data transmission
 * happens until the caller explicitly passes a connected socket and calls this.
 */
bool l3p1_tcp_sink(const uint8_t *packet, uint32_t packet_size, void *context);

#endif
