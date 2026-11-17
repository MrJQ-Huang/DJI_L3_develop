#define _POSIX_C_SOURCE 200809L
#include "l3_video_control.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include <time.h>

L3VideoControlResult l3_video_source_read(E_DjiMountPosition mount)
{
    L3VideoControlResult r = {.previous_source = -1, .current_source = -1};
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
    T_DjiCameraManagerRangeList range = {0};
    r.code = DjiCameraManager_GetStreamSourceRange(mount, &range);
    if (r.code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) return r;
    if (range.size == 0 || range.size > sizeof(range.streamSource) / sizeof(range.streamSource[0])) {
        r.code = DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
        return r;
    }
    bool supported = false, original_supported = false;
    for (unsigned int i = 0; i < range.size; ++i) {
        supported |= (unsigned int)range.streamSource[i] == source;
        original_supported |= (int)range.streamSource[i] == r.previous_source;
    }
    if (!supported || !original_supported) { r.code = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT; return r; }
    if ((int)source == r.current_source) { r.confirmed = true; return r; }
    r.write_attempted = true;
    r.code = DjiCameraManager_SetStreamSource(mount, (E_DjiCameraManagerStreamSource)source);
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
