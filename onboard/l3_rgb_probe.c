#include "l3_rgb_probe.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "dji_camera_manager.h"
#include "dji_error.h"
#include "dji_liveview.h"

static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drained = PTHREAD_COND_INITIALIZER;

/* Lifecycle state is protected by control_lock; the SDK callback never takes
 * it. Never hold callback_lock while calling into the SDK or the user sink. */
static bool attempted;
static bool stop_needed;
static unsigned callback_slot;
static E_DjiLiveViewCameraPosition position;
static L3RgbProbeSink active_sink;
static void *active_user;
static size_t in_flight;

static void receive_stream(unsigned slot, E_DjiLiveViewCameraPosition camera,
                           const uint8_t *data, uint32_t size)
{
    if (data == NULL || size == 0) {
        return;
    }

    pthread_mutex_lock(&callback_lock);
    if (active_sink == NULL || slot != callback_slot || camera != position) {
        pthread_mutex_unlock(&callback_lock);
        return;
    }
    L3RgbProbeSink sink = active_sink;
    void *user = active_user;
    ++in_flight;
    pthread_mutex_unlock(&callback_lock);

    sink(data, size, user);

    pthread_mutex_lock(&callback_lock);
    if (--in_flight == 0) {
        pthread_cond_signal(&drained);
    }
    pthread_mutex_unlock(&callback_lock);
}

/* Keep a late callback from the immediately preceding subscription out of
 * the new sink. Reuse a slot only after the SDK has acknowledged its stop. */
static void receive_first(E_DjiLiveViewCameraPosition camera, const uint8_t *data, uint32_t size)
{
    receive_stream(0, camera, data, size);
}

static void receive_second(E_DjiLiveViewCameraPosition camera, const uint8_t *data, uint32_t size)
{
    receive_stream(1, camera, data, size);
}

static void disable_delivery(void)
{
    pthread_mutex_lock(&callback_lock);
    active_sink = NULL;
    active_user = NULL;
    while (in_flight != 0) {
        pthread_cond_wait(&drained, &callback_lock);
    }
    pthread_mutex_unlock(&callback_lock);
}

static T_DjiReturnCode start_probe(E_DjiMountPosition mount,
                                  L3RgbProbeSink sink, void *user, bool restart)
{
    E_DjiLiveViewCameraPosition camera;
    switch (mount) {
    case DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1:
        camera = DJI_LIVEVIEW_CAMERA_POSITION_NO_1;
        break;
    case DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2:
        camera = DJI_LIVEVIEW_CAMERA_POSITION_NO_2;
        break;
    case DJI_MOUNT_POSITION_PAYLOAD_PORT_NO3:
        camera = DJI_LIVEVIEW_CAMERA_POSITION_NO_3;
        break;
    default:
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    if (sink == NULL) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    }
    if (pthread_mutex_trylock(&control_lock) != 0) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_BUSY;
    }

    T_DjiReturnCode rc = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT_IN_CURRENT_STATE;
    if (attempted != restart || stop_needed) {
        goto done;
    }
    E_DjiCameraType type = DJI_CAMERA_TYPE_UNKNOWN;
    rc = DjiCameraManager_GetCameraType(mount, &type);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        goto done;
    }
    if (type != DJI_CAMERA_TYPE_L3) {
        rc = DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
        goto done;
    }

    attempted = true;
    stop_needed = true;
    pthread_mutex_lock(&callback_lock);
    if (restart) callback_slot ^= 1U;
    position = camera;
    active_sink = sink;
    active_user = user;
    pthread_mutex_unlock(&callback_lock);

    rc = DjiLiveview_StartStream(camera, DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT,
                                callback_slot == 0 ? receive_first : receive_second);
    if (rc != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
        /* Do not assume an SDK error excludes partial startup. The caller
         * must still call stop to request cleanup of this attempted stream. */
        disable_delivery();
    }

done:
    pthread_mutex_unlock(&control_lock);
    return rc;
}

T_DjiReturnCode l3_rgb_probe_start(E_DjiMountPosition mount, L3RgbProbeSink sink, void *user)
{
    return start_probe(mount, sink, user, false);
}

T_DjiReturnCode l3_rgb_probe_restart(E_DjiMountPosition mount, L3RgbProbeSink sink, void *user)
{
    return start_probe(mount, sink, user, true);
}

T_DjiReturnCode l3_rgb_probe_stop(void)
{
    if (pthread_mutex_trylock(&control_lock) != 0) {
        return DJI_ERROR_SYSTEM_MODULE_CODE_BUSY;
    }

    disable_delivery();
    T_DjiReturnCode rc = DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    if (stop_needed) {
        rc = DjiLiveview_StopStream(position, DJI_LIVEVIEW_CAMERA_SOURCE_DEFAULT);
        if (rc == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) {
            stop_needed = false;
        }
    }
    pthread_mutex_unlock(&control_lock);
    return rc;
}
