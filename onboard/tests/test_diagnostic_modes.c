/* Exercise mode isolation with SDK stubs; never link the DJI SDK library. */
#define main unused_metadata_main
#include "../metadata_inspect.c"
#undef main
#include <assert.h>

static unsigned int liveview_inits, video_starts, video_stops, liveview_deinits, point_runs;
static bool fail_video_start;
static bool wrong_camera, metadata_error;
static uint8_t range_size = 2;
static unsigned int range_queries;
static uint8_t current_source = DJI_CAMERA_MANAGER_SOURCE_WIDE_CAM;

T_DjiReturnCode DjiCameraManager_GetCameraType(E_DjiMountPosition mount, E_DjiCameraType *type)
{
    assert(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    *type = wrong_camera ? DJI_CAMERA_TYPE_UNKNOWN : DJI_CAMERA_TYPE_L3;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_GetStreamSourceRange(E_DjiMountPosition mount,
                                                      T_DjiCameraManagerRangeList *range)
{
    (void)mount;
    ++range_queries;
    range->size = range_size;
    range->streamSource[0] = DJI_CAMERA_MANAGER_SOURCE_VISIBLE_CAM; /* SDK's incorrect static table. */
    range->streamSource[1] = (E_DjiCameraManagerStreamSource)14;
    return metadata_error ? DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT : DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode DjiCameraManager_GetCurrentCameraStatus(E_DjiMountPosition mount,
                                                       T_DjiCameraCurrentCameraStatus *camera)
{
    (void)mount;
    camera->liveview_source_stream = current_source;
    return metadata_error ? DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT : DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiCameraManagerUtil_ActionSync(
    E_DjiMountPosition mount, uint8_t command_set, uint8_t command_id,
    const uint8_t *data, uint32_t length, void *response_info, uint8_t *ack, uint16_t capacity)
{
    (void)mount; (void)command_set; (void)command_id; (void)data;
    (void)length; (void)response_info; (void)ack; (void)capacity;
    assert(!"no source writes in diagnostic/read-only tests");
    return DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
}
T_DjiReturnCode DjiLiveview_RequestIntraframeFrameData(E_DjiLiveViewCameraPosition mount, E_DjiLiveViewCameraSource source)
{
    (void)mount; (void)source;
    assert(!"no source changes in diagnostic/read-only tests");
    return DJI_ERROR_SYSTEM_MODULE_CODE_NONSUPPORT;
}
T_DjiReturnCode DjiLiveview_Init(void) { ++liveview_inits; return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS; }
T_DjiReturnCode DjiLiveview_Deinit(void) { ++liveview_deinits; return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS; }
T_DjiReturnCode l3_rgb_probe_start(E_DjiMountPosition mount, L3RgbProbeSink sink, void *user)
{
    assert(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    ++video_starts;
    if (fail_video_start) return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    const uint8_t synthetic[] = {0, 0, 0, 1, 0x65, 0x80};
    sink(synthetic, sizeof(synthetic), user);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode l3_rgb_probe_stop(void) { ++video_stops; return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS; }
L3PointProbeOutcome l3_point_probe_run(const L3PointProbeConfig *config,
                                      L3PointProbeReport *report,
                                      L3PointProbeController **retained)
{
    ++point_runs;
    assert(config->continuous && config->confirmed_no_existing_recording);
    assert(config->duration_ms == 1000);
    uint8_t synthetic[44] = {255, 255, 255, 255};
    synthetic[16] = 16;
    assert(config->sink(synthetic, sizeof(synthetic), config->sink_context));
    memset(report, 0, sizeof(*report));
    report->stop_command_confirmed = true;
    report->cleanup_safe = true;
    report->reader_report.packets_delivered = 1;
    *retained = NULL;
    return L3_POINT_PROBE_OK;
}

int main(void)
{
    assert(run_sdk_diagnostic((E_DjiMountPosition)0, 1, false) == 0);
    assert(liveview_inits == 0 && point_runs == 0);
    assert(run_sdk_diagnostic(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, true) == 0);
    assert(liveview_inits == 1 && video_starts == 1 && video_stops == 1 && liveview_deinits == 1);
    assert(point_runs == 0);

    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, L3_STREAM_POINTS, -1, false) == 0);
    assert(point_runs == 1);
    assert(liveview_inits == 1 && video_starts == 1 && video_stops == 1 && liveview_deinits == 1);
    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, L3_STREAM_BOTH, -1, false) == 0);
    assert(point_runs == 2);
    assert(liveview_inits == 2 && video_starts == 2 && video_stops == 2 && liveview_deinits == 2);

    /* Video forwarding must never invoke the point-recording controller. */
    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, L3_STREAM_VIDEO, -1, false) == 0);
    assert(point_runs == 2);
    assert(liveview_inits == 3 && video_starts == 3 && video_stops == 3 && liveview_deinits == 3);

    assert(report_video_info(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, "test") == 0);
    range_size = 17; /* Never traverse a vendor count beyond the 16-entry array. */
    assert(report_video_info(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, "test") == 1);
    range_size = 0;
    assert(report_video_info(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, "test") == 1);
    range_size = 2;
    metadata_error = true;
    assert(report_video_info(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, "test") == 1);
    metadata_error = false;
    wrong_camera = true;
    unsigned int before_queries = range_queries;
    assert(report_video_info(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, "test") == 1);
    assert(range_queries == before_queries);
    wrong_camera = false;

    assert(!check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 14));
    assert(check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, DJI_CAMERA_MANAGER_SOURCE_WIDE_CAM));
    assert(!check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, DJI_CAMERA_MANAGER_SOURCE_DEFAULT_CAM));
    assert(!check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, DJI_CAMERA_MANAGER_SOURCE_VISIBLE_CAM));
    current_source = 14;
    assert(check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 14));
    current_source = DJI_CAMERA_MANAGER_SOURCE_WIDE_CAM;
    wrong_camera = true;
    assert(!check_video_source(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, DJI_CAMERA_MANAGER_SOURCE_WIDE_CAM));
    wrong_camera = false;

    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, L3_STREAM_VIDEO, 14, false) == 1);
    assert(point_runs == 2 && liveview_inits == 3 && video_starts == 3);

    fail_video_start = true;
    assert(run_sdk_diagnostic(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, true) == 1);
    assert(point_runs == 2);
    assert(liveview_inits == 4 && video_starts == 4 && video_stops == 4 && liveview_deinits == 4);
    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, L3_STREAM_VIDEO, -1, false) == 1);
    assert(point_runs == 2 && video_stops == 5 && liveview_deinits == 5);
    stop_requested = 1;
    assert(run_sdk_diagnostic((E_DjiMountPosition)0, 1, false) == 1);
    puts("PASS: video/point mode isolation, metadata bounds/type, cleanup and cancellation (SDK stubs)");
    return 0;
}
