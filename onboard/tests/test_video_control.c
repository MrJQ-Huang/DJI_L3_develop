/* No vendor binary/hardware: reject unsupported/unconfirmed source changes. */
#include "l3_video_control.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include <assert.h>
#include <stdio.h>

static bool wrong_type, fail_read, fail_range, ignore_write, change_on_error;
static unsigned writes, reads, delay_reads;
static int current = 14, target = 14;
static uint8_t range_size = 2;
static T_DjiReturnCode write_code;

T_DjiReturnCode DjiCameraManager_GetCameraType(E_DjiMountPosition mount, E_DjiCameraType *type)
{
    assert(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2);
    *type = wrong_type ? DJI_CAMERA_TYPE_UNKNOWN : DJI_CAMERA_TYPE_L3;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_GetCurrentCameraStatus(E_DjiMountPosition mount, T_DjiCameraCurrentCameraStatus *status)
{
    (void)mount;
    if (fail_read) return DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT;
    if (++reads > delay_reads && !ignore_write) current = target;
    status->liveview_source_stream = (uint8_t)current;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_GetStreamSourceRange(E_DjiMountPosition mount, T_DjiCameraManagerRangeList *range)
{
    (void)mount;
    range->size = range_size;
    range->streamSource[0] = DJI_CAMERA_MANAGER_SOURCE_VISIBLE_CAM;
    range->streamSource[1] = (E_DjiCameraManagerStreamSource)14;
    return fail_range ? DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT : DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_SetStreamSource(E_DjiMountPosition mount, E_DjiCameraManagerStreamSource source)
{
    (void)mount;
    ++writes;
    if (write_code == 0 || change_on_error) target = source;
    return write_code;
}

int main(void)
{
    E_DjiMountPosition mount = DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2;
    wrong_type = true;
    assert(!l3_video_source_set(mount, 7).confirmed && writes == 0);
    wrong_type = false; fail_read = true;
    assert(l3_video_source_set(mount, 7).current_source == -1 && writes == 0);
    fail_read = false; fail_range = true;
    assert(!l3_video_source_set(mount, 7).confirmed && writes == 0);
    fail_range = false; range_size = 17;
    assert(!l3_video_source_set(mount, 7).confirmed && writes == 0);
    range_size = 0;
    assert(!l3_video_source_set(mount, 7).confirmed && writes == 0);
    range_size = 2;
    assert(!l3_video_source_set(mount, 1).confirmed && writes == 0);
    assert(l3_video_source_set(mount, 14).confirmed && writes == 0);
    write_code = UINT64_C(0x00000021000000e0);
    L3VideoControlResult r = l3_video_source_set(mount, 7);
    assert(!r.confirmed && r.write_attempted && r.current_source == 14 && r.code == write_code);
    write_code = 0; ignore_write = true;
    r = l3_video_source_set(mount, 7);
    assert(!r.confirmed && r.current_source == 14 && r.code == DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT);
    ignore_write = false; target = 14; reads = 0; delay_reads = 3;
    r = l3_video_source_set(mount, 7);
    assert(r.confirmed && r.previous_source == 14 && r.current_source == 7 && reads == 4);
    write_code = UINT64_C(0x00000021000000e0); change_on_error = true;
    r = l3_video_source_set(mount, 14);
    assert(!r.confirmed && r.current_source == 14 && r.code == write_code);
    puts("PASS: exact L3 on selected mount, bounded range, ACK + readback, unsupported/late/ambiguous results");
}
