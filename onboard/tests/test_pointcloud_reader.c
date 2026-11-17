/* SDK stubs: cancellation and continuous packet counts without hardware. */
#include "l3_pointcloud_psdk.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include "dji_mop_channel.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned received, delivered, created, closed, destroyed;
static bool cancelled;
T_DjiReturnCode DjiCameraManager_GetCameraType(E_DjiMountPosition m, E_DjiCameraType *type)
{ (void)m; *type = DJI_CAMERA_TYPE_L3; return 0; }
T_DjiReturnCode DjiMopChannel_Create(T_DjiMopChannelHandle *h, E_DjiMopChannelTransType type)
{ assert(type == DJI_MOP_CHANNEL_TRANS_UNRELIABLE); ++created; *h = (void *)(uintptr_t)1; return 0; }
T_DjiReturnCode DjiMopChannel_Connect(T_DjiMopChannelHandle h, E_DjiChannelAddress a, uint16_t id)
{ (void)h; assert(a != DJI_CHANNEL_ADDRESS_UNKNOWN && id == 49154); return 0; }
T_DjiReturnCode DjiMopChannel_Close(T_DjiMopChannelHandle h)
{ (void)h; ++closed; return 0; }
T_DjiReturnCode DjiMopChannel_Destroy(T_DjiMopChannelHandle h)
{ (void)h; ++destroyed; return 0; }
T_DjiReturnCode DjiMopChannel_RecvData(T_DjiMopChannelHandle h, uint8_t *data, uint32_t cap, uint32_t *size)
{
    (void)h; assert(cap >= 44); ++received;
    memset(data, 0, 44); memset(data, 255, 4); data[16] = 16;
    *size = received == 1 ? 0 : 44; return 0;
}
static bool stop(void *p) { (void)p; return cancelled || delivered == 40; }
static bool sink(const uint8_t *data, uint32_t size, void *p)
{ (void)p; assert(data[16] == 16 && size == 44); ++delivered; return true; }
static void reset(void)
{ received = delivered = created = closed = destroyed = 0; cancelled = false; }
int main(void)
{
    L3PointCloudReadReport report;
    reset();
    assert(l3_pointcloud_psdk_read_until(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, stop, sink, NULL, &report) == L3_POINTCLOUD_SINK_STOPPED);
    assert(delivered == 40 && report.packets_delivered == 40 && report.malformed_packets == 1);
    assert(received == 41 && created == 1 && closed == 1 && destroyed == 1);
    reset(); cancelled = true;
    assert(l3_pointcloud_psdk_read_until(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, stop, sink, NULL, &report) == L3_POINTCLOUD_SINK_STOPPED);
    assert(created == 0 && received == 0);
    reset();
    assert(l3_pointcloud_psdk_read(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 60, 30, sink, NULL, &report) == L3_POINTCLOUD_OK);
    assert(delivered == 30 && received == 31 && closed == 1 && destroyed == 1);
    puts("PASS continuous packets, pre-connect cancellation, probe limit and cleanup");
    return 0;
}
