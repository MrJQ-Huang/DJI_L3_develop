#include "l3_rgb_probe.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include "dji_liveview.h"

#include <assert.h>
#include <stdbool.h>

static DjiLiveview_H264Callback callback;
static bool stop_fails;
static const E_DjiMountPosition mount = DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1;
static const E_DjiLiveViewCameraPosition camera = DJI_LIVEVIEW_CAMERA_POSITION_NO_1;

T_DjiReturnCode DjiCameraManager_GetCameraType(E_DjiMountPosition pos, E_DjiCameraType *type)
{
    assert(pos == mount);
    *type = DJI_CAMERA_TYPE_L3;
    return 0;
}

T_DjiReturnCode DjiLiveview_StartStream(E_DjiLiveViewCameraPosition pos, E_DjiLiveViewCameraSource source,
                                      DjiLiveview_H264Callback handler)
{
    assert(pos == camera && source == DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT);
    callback = handler;
    return 0;
}

T_DjiReturnCode DjiLiveview_StopStream(E_DjiLiveViewCameraPosition pos, E_DjiLiveViewCameraSource source)
{
    assert(pos == camera && source == DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT);
    return stop_fails ? DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR : 0;
}

static void count_frame(const uint8_t *data, uint32_t size, void *user)
{
    assert(data[0] == 0x65 && size == 1);
    ++*(unsigned *)user;
}

int main(void)
{
    unsigned old_frames = 0, new_frames = 0;
    uint8_t data[] = {0x65};
    assert(l3_rgb_probe_start(mount, count_frame, &old_frames) == 0);
    DjiLiveview_H264Callback old_callback = callback;
    callback(camera, data, sizeof(data));
    assert(old_frames == 1);

    stop_fails = true;
    assert(l3_rgb_probe_stop() != 0);
    assert(l3_rgb_probe_restart(mount, count_frame, &new_frames) != 0);
    callback(camera, data, sizeof(data));
    assert(old_frames == 1);

    stop_fails = false;
    assert(l3_rgb_probe_stop() == 0);
    assert(l3_rgb_probe_start(mount, count_frame, &new_frames) != 0);
    assert(l3_rgb_probe_restart(mount, count_frame, &new_frames) == 0);
    old_callback(camera, data, sizeof(data));
    assert(new_frames == 0);
    callback(camera, data, sizeof(data));
    assert(new_frames == 1);
    assert(l3_rgb_probe_stop() == 0);
    return 0;
}
