/* No vendor binary/hardware: reject unsupported/unconfirmed source changes. */
#include "l3_video_control.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool wrong_type, fail_read, ignore_write, change_on_error;
static unsigned writes, reads, delay_reads;
static int current = 14, target = 14;
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
    range->size = 2;
    range->streamSource[0] = DJI_CAMERA_MANAGER_SOURCE_VISIBLE_CAM;
    range->streamSource[1] = (E_DjiCameraManagerStreamSource)14;
    assert(!"PSDK's static [7,14] table must not decide L3 source writes");
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_SetStreamSource(E_DjiMountPosition mount, E_DjiCameraManagerStreamSource source)
{
    (void)mount; (void)source;
    assert(!"public PSDK L3 feature gate rejects source writes locally");
    return DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
}
T_DjiReturnCode DjiCameraManagerUtil_ActionSync(
    E_DjiMountPosition mount, uint8_t command_set, uint8_t command_id,
    const uint8_t *data, uint32_t length, void *response_info, uint8_t *ack, uint16_t capacity)
{
    assert(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2);
    assert(command_set == 2 && command_id == 9 && length == 11);
    assert(data[0] == 1 || data[0] == 14); /* Never send the erroneous visible ID 7. */
    const uint8_t tail[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    assert(memcmp(data + 1, tail, sizeof(tail)) == 0);
    assert((uintptr_t)response_info % 8 == 0 && capacity == 2);
    memset(response_info, 0, 40); memset(ack, 0, capacity);
    ++writes;
    if (write_code == 0 || change_on_error) target = data[0];
    return write_code;
}

int main(void)
{
    E_DjiMountPosition mount = DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2;
    assert(!l3_video_source_set((E_DjiMountPosition)0, 1).confirmed && writes == 0);
    assert(!l3_video_source_set((E_DjiMountPosition)4, 1).confirmed && writes == 0);
    wrong_type = true;
    assert(!l3_video_source_set(mount, 1).confirmed && writes == 0);
    wrong_type = false; fail_read = true;
    assert(l3_video_source_set(mount, 1).current_source == -1 && writes == 0);
    fail_read = false;
    assert(!l3_video_source_set(mount, 7).confirmed && writes == 0);
    assert(!l3_video_source_set(mount, 0).confirmed && writes == 0);
    assert(!l3_video_source_set(mount, 257).confirmed && writes == 0);
    current = target = 7;
    assert(!l3_video_source_set(mount, 1).confirmed && writes == 0);
    current = target = 14;
    assert(l3_video_source_set(mount, 14).confirmed && writes == 0);
    write_code = UINT64_C(0x00000021000000e0);
    L3VideoControlResult r = l3_video_source_set(mount, 1);
    assert(!r.confirmed && r.write_attempted && r.current_source == 14 && r.code == write_code);
    write_code = 0; ignore_write = true;
    r = l3_video_source_set(mount, 1);
    assert(!r.confirmed && r.current_source == 14 && r.code == DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT);
    ignore_write = false; target = 14; reads = 0; delay_reads = 3;
    r = l3_video_source_set(mount, 1);
    assert(r.confirmed && r.previous_source == 14 && r.current_source == 1 && reads == 4);
    r = l3_video_source_set(mount, 14);
    assert(r.confirmed && r.previous_source == 1 && r.current_source == 14);
    write_code = UINT64_C(0x00000021000000e0); change_on_error = true;
    r = l3_video_source_set(mount, 1);
    assert(!r.confirmed && r.current_source == 1 && r.code == write_code);
    puts("PASS: exact L3/mount, 1<->14 selector packet, invalid IDs, ACK + actual readback, late/ambiguous results");
}
