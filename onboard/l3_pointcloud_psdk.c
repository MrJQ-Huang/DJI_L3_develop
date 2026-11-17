#include "l3_pointcloud_psdk.h"

#include <stddef.h>
#include <stdlib.h>

#include "dji_camera_manager.h"
#include "dji_mop_channel.h"

enum { L3_POINTCLOUD_MOP_CHANNEL = 49154, L3_POINTCLOUD_BUFFER_SIZE = 512 * 1024 };

_Static_assert(sizeof(T_DjiCameraManagerPointCloudHeader) == 20, "DJI point cloud header changed");
_Static_assert(offsetof(T_DjiCameraManagerColorPointCloud, points) == 28, "DJI point offset changed");
_Static_assert(sizeof(T_DjiCameraManagerPointXYZRGBInfo) == 16, "DJI point layout changed");

static E_DjiChannelAddress address_for_mount(E_DjiMountPosition mount)
{
    switch (mount) {
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO1: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO1;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO2: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO2;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO3: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO3;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO4: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO4;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO5: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO5;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO6: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO6;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO7: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO7;
        case DJI_MOUNT_POSITION_EXTENSION_PORT_V2_NO8: return DJI_CHANNEL_ADDRESS_EXTENSION_PORT_V2_NO8;
        default: return DJI_CHANNEL_ADDRESS_UNKNOWN;
    }
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) |
           ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static bool is_complete_packet(const uint8_t *buffer, uint32_t length)
{
    const size_t prefix = offsetof(T_DjiCameraManagerColorPointCloud, points);
    const size_t point_size = sizeof(T_DjiCameraManagerPointXYZRGBInfo);
    if (length < prefix || length > L3_POINTCLOUD_BUFFER_SIZE) {
        return false;
    }

    /* Follow the native packed layout used by DJI on little-endian Manifold 3.
     * Validate one full packet per receive. Preserve the CRC fields as-is;
     * the public SDK header does not specify their calculation. */
    const size_t header = offsetof(T_DjiCameraManagerColorPointCloud, pointCloudHeader);
    const uint32_t flag = read_u32_le(buffer + header + offsetof(T_DjiCameraManagerPointCloudHeader, flag));
    const uint32_t data_bytes = read_u32_le(buffer + header + offsetof(T_DjiCameraManagerPointCloudHeader, dataByte));
    return flag == UINT32_MAX && data_bytes != 0 && data_bytes % point_size == 0 &&
           data_bytes == length - prefix;
}

static L3PointCloudResult read_packets(E_DjiMountPosition mount,
                                         uint32_t max_receive_attempts,
                                         uint32_t max_packets,
                                         L3PointCloudStopRequested stop_requested,
                                         L3PointCloudPacketSink sink,
                                         void *user_context,
                                         L3PointCloudReadReport *report)
{
    L3PointCloudReadReport stats = {0};
    L3PointCloudResult result = L3_POINTCLOUD_INVALID_ARGUMENT;
    T_DjiMopChannelHandle channel = NULL;
    uint8_t *buffer = NULL;
    bool created = false;
    bool connected = false;
    const E_DjiChannelAddress address = address_for_mount(mount);
    E_DjiCameraType camera_type = DJI_CAMERA_TYPE_UNKNOWN;
    if (address == DJI_CHANNEL_ADDRESS_UNKNOWN || sink == NULL ||
        (stop_requested == NULL && (max_receive_attempts == 0 || max_packets == 0))) {
        goto cleanup;
    }

    stats.sdk_error = DjiCameraManager_GetCameraType(mount, &camera_type);
    if (stats.sdk_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        result = L3_POINTCLOUD_CAMERA_QUERY_FAILED;
        goto cleanup;
    }
    if (camera_type != DJI_CAMERA_TYPE_L3) {
        result = L3_POINTCLOUD_NOT_L3;
        goto cleanup;
    }

    if (stop_requested != NULL && stop_requested(user_context)) {
        result = L3_POINTCLOUD_SINK_STOPPED;
        goto cleanup;
    }
    buffer = malloc(L3_POINTCLOUD_BUFFER_SIZE);
    if (buffer == NULL) {
        result = L3_POINTCLOUD_NO_MEMORY;
        goto cleanup;
    }
    stats.sdk_error = DjiMopChannel_Create(&channel, DJI_MOP_CHANNEL_TRANS_UNRELIABLE);
    if (stats.sdk_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS || channel == NULL) {
        result = L3_POINTCLOUD_MOP_CREATE_FAILED;
        goto cleanup;
    }
    created = true;

    if (stop_requested != NULL && stop_requested(user_context)) {
        result = L3_POINTCLOUD_SINK_STOPPED;
        goto cleanup;
    }
    stats.sdk_error = DjiMopChannel_Connect(channel, address, L3_POINTCLOUD_MOP_CHANNEL);
    if (stats.sdk_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        result = L3_POINTCLOUD_MOP_CONNECT_FAILED;
        goto cleanup;
    }
    connected = true;

    result = L3_POINTCLOUD_READ_LIMIT;
    while (stop_requested != NULL || stats.receive_attempts < max_receive_attempts) {
        if (stop_requested != NULL && stop_requested(user_context)) {
            result = L3_POINTCLOUD_SINK_STOPPED;
            break;
        }
        uint32_t received = 0;
        if (stats.receive_attempts < UINT32_MAX) ++stats.receive_attempts;
        stats.sdk_error = DjiMopChannel_RecvData(channel, buffer, L3_POINTCLOUD_BUFFER_SIZE, &received);
        if (stats.sdk_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            result = L3_POINTCLOUD_MOP_RECEIVE_FAILED;
            break;
        }
        if (stop_requested != NULL && stop_requested(user_context)) {
            result = L3_POINTCLOUD_SINK_STOPPED;
            break;
        }
        if (!is_complete_packet(buffer, received)) {
            if (stats.malformed_packets < UINT32_MAX) ++stats.malformed_packets;
            continue;
        }
        if (stats.packets_delivered < UINT32_MAX) ++stats.packets_delivered;
        if (!sink(buffer, received, user_context)) {
            result = L3_POINTCLOUD_SINK_STOPPED;
            break;
        }
        if (stop_requested == NULL && stats.packets_delivered == max_packets) {
            result = L3_POINTCLOUD_OK;
            break;
        }
    }

cleanup:
    if (connected) {
        stats.close_error = DjiMopChannel_Close(channel);
    }
    if (created) {
        stats.destroy_error = DjiMopChannel_Destroy(channel);
    }
    free(buffer);
    if (result <= L3_POINTCLOUD_READ_LIMIT &&
        (stats.close_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ||
         stats.destroy_error != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS)) {
        result = L3_POINTCLOUD_MOP_CLEANUP_FAILED;
    }
    if (report != NULL) {
        *report = stats;
    }
    return result;
}

L3PointCloudResult l3_pointcloud_psdk_read(E_DjiMountPosition mount,
                                         uint32_t max_receive_attempts,
                                         uint32_t max_packets,
                                         L3PointCloudPacketSink sink,
                                         void *user_context,
                                         L3PointCloudReadReport *report)
{
    return read_packets(mount, max_receive_attempts, max_packets, NULL, sink, user_context, report);
}

L3PointCloudResult l3_pointcloud_psdk_read_until(E_DjiMountPosition mount,
                                               L3PointCloudStopRequested stop_requested,
                                               L3PointCloudPacketSink sink,
                                               void *user_context,
                                               L3PointCloudReadReport *report)
{
    if (stop_requested == NULL) {
        if (report != NULL) *report = (L3PointCloudReadReport){0};
        return L3_POINTCLOUD_INVALID_ARGUMENT;
    }
    return read_packets(mount, 0, 0, stop_requested, sink, user_context, report);
}
