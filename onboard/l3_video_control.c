#define _POSIX_C_SOURCE 200809L
#include "l3_video_control.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include <time.h>

/* Internal transport ABI of the hash-pinned ARM64 PSDK 3.18 archive (checked
 * by CMake). Use the SDK's own camera addressing, ACK parser and timeouts.
 * SetStreamSource's local L3 feature/range tables prevent its public wrapper
 * from sending the correct source. Keep this adapter limited to L3 video.
 * Command 02/09 and its 11-byte payload are the SDK's source-selector format;
 * no camera work mode, recording, quality or flight command is sent here. */
extern T_DjiReturnCode DjiCameraManagerUtil_ActionSync(
    E_DjiMountPosition mount, uint8_t command_set, uint8_t command_id,
    const uint8_t *data, uint32_t length, void *response_info,
    uint8_t *ack, uint16_t ack_capacity);

static bool valid_source(unsigned int source)
{
    return source == L3_VIDEO_SOURCE_VISIBLE || source == L3_VIDEO_SOURCE_POINT_CLOUD;
}

static T_DjiReturnCode set_l3_source(E_DjiMountPosition mount, unsigned int source)
{
    const uint8_t payload[11] = {(uint8_t)source, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    uint64_t response_info[5] = {0}; /* SDK command metadata: 40 bytes, aligned. */
    uint8_t ack[2] = {0};
    return DjiCameraManagerUtil_ActionSync(mount, 2, 9, payload, sizeof(payload),
                                           response_info, ack, sizeof(ack));
}

L3VideoControlResult l3_video_source_read(E_DjiMountPosition mount)
{
    L3VideoControlResult r = {.previous_source = -1, .current_source = -1};
    if (mount < DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1 || mount > DJI_MOUNT_POSITION_PAYLOAD_PORT_NO3) {
        r.code = DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
        return r;
    }
    E_DjiCameraType type = DJI_CAMERA_TYPE_UNKNOWN;
    r.code = DjiCameraManager_GetCameraType(mount, &type);
    if (r.code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) return r;
    if (type != DJI_CAMERA_TYPE_L3) { r.code = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT; return r; }
    T_DjiCameraCurrentCameraStatus status = {0};
    r.code = DjiCameraManager_GetCurrentCameraStatus(mount, &status);
    if (r.code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        r.previous_source = r.current_source = status.liveview_source_stream;
        r.confirmed = true;
    }
    return r;
}

L3VideoControlResult l3_video_source_set(E_DjiMountPosition mount, unsigned int source)
{
    L3VideoControlResult r = l3_video_source_read(mount);
    if (!r.confirmed) return r;
    r.confirmed = false;
    if (!valid_source(source) || !valid_source((unsigned int)r.current_source)) {
        r.code = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
        return r;
    }
    if ((int)source == r.current_source) { r.confirmed = true; return r; }
    r.write_attempted = true;
    r.code = set_l3_source(mount, source);
    T_DjiReturnCode set_code = r.code;
    /* ACK alone is insufficient. Check actual state even on a failed ACK. */
    for (unsigned int i = 0; i < 5; ++i) {
        L3VideoControlResult observed = l3_video_source_read(mount);
        r.current_source = observed.current_source;
        if (set_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) break;
        if (observed.confirmed && observed.current_source == (int)source) {
            r.confirmed = true;
            return r;
        }
        if (i != 4) {
            struct timespec delay = {.tv_nsec = 100000000};
            nanosleep(&delay, NULL);
        }
    }
    if (set_code == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) r.code = DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
    return r;
}
