/* Exercise mode isolation with SDK stubs; never link the DJI SDK library. */
#define main unused_metadata_main
#include "../metadata_inspect.c"
#undef main
#include <assert.h>

static unsigned int liveview_inits, video_starts, video_stops, liveview_deinits, point_runs;
static bool fail_video_start;

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

    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, false) == 0);
    assert(point_runs == 1);
    assert(liveview_inits == 1 && video_starts == 1 && video_stops == 1 && liveview_deinits == 1);
    assert(run_live_stream(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, "127.0.0.1", 0, 0, true) == 0);
    assert(point_runs == 2);
    assert(liveview_inits == 2 && video_starts == 2 && video_stops == 2 && liveview_deinits == 2);

    fail_video_start = true;
    assert(run_sdk_diagnostic(DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1, 1, true) == 1);
    assert(point_runs == 2);
    assert(liveview_inits == 3 && video_starts == 3 && video_stops == 3 && liveview_deinits == 3);
    stop_requested = 1;
    assert(run_sdk_diagnostic((E_DjiMountPosition)0, 1, false) == 1);
    puts("PASS: diagnostic/point-only mode isolation, video cleanup and cancellation (SDK stubs)");
    return 0;
}
