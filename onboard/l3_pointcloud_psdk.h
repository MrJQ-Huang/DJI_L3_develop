#ifndef L3_POINTCLOUD_PSDK_H
#define L3_POINTCLOUD_PSDK_H

#include "dji_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    L3_POINTCLOUD_OK = 0,             /* max_packets valid packets delivered */
    L3_POINTCLOUD_SINK_STOPPED,        /* sink asked to stop */
    L3_POINTCLOUD_READ_LIMIT,          /* attempt budget exhausted before max_packets */
    L3_POINTCLOUD_INVALID_ARGUMENT,
    L3_POINTCLOUD_CAMERA_QUERY_FAILED,
    L3_POINTCLOUD_NOT_L3,
    L3_POINTCLOUD_NO_MEMORY,
    L3_POINTCLOUD_MOP_CREATE_FAILED,
    L3_POINTCLOUD_MOP_CONNECT_FAILED,
    L3_POINTCLOUD_MOP_RECEIVE_FAILED,
    L3_POINTCLOUD_MOP_CLEANUP_FAILED,
} L3PointCloudResult;

typedef struct {
    uint32_t receive_attempts;
    uint32_t packets_delivered;
    uint32_t malformed_packets;
    T_DjiReturnCode sdk_error;     /* camera query, create, connect or receive failure */
    T_DjiReturnCode close_error;   /* zero if successful or not called */
    T_DjiReturnCode destroy_error; /* zero if successful or not called */
} L3PointCloudReadReport;

/* The original DJI packet is valid only during this callback. Copy it to keep it.
 * Return true to continue or false to stop. Keep the callback nonblocking. */
typedef bool (*L3PointCloudPacketSink)(const uint8_t *packet,
                                      uint32_t packet_size,
                                      void *user_context);
typedef bool (*L3PointCloudStopRequested)(void *user_context);

/* Read-only L3 adapter for PSDK 3.18.0. Caller owns the PSDK application and
 * must initialize CameraManager and MOP before this call. This adapter does
 * not start/stop L3 recording; recording must already be active for data.
 * Follow DJI's L3 subscription instructions regarding DJI Pilot 2.
 *
 * Run on a worker. max_receive_attempts (including malformed/error reads)
 * and max_packets must be nonzero. There is one connect attempt, no retry.
 * The SDK connect/receive calls have no documented timeout/cancel argument;
 * these are count limits, not deadlines. No cancellation while an SDK call
 * is blocked is promised. Optional report is populated when this returns. */
L3PointCloudResult l3_pointcloud_psdk_read(E_DjiMountPosition mount,
                                         uint32_t max_receive_attempts,
                                         uint32_t max_packets,
                                         L3PointCloudPacketSink sink,
                                         void *user_context,
                                         L3PointCloudReadReport *report);

/* Continuous read: checks stop between SDK calls; cannot interrupt blocked
 * Connect/Recv/Close. Caller still owns recording and an independent Stop.
 * A requested stop returns L3_POINTCLOUD_SINK_STOPPED. */
L3PointCloudResult l3_pointcloud_psdk_read_until(E_DjiMountPosition mount,
                                               L3PointCloudStopRequested stop_requested,
                                               L3PointCloudPacketSink sink,
                                               void *user_context,
                                               L3PointCloudReadReport *report);

#ifdef __cplusplus
}
#endif

#endif
