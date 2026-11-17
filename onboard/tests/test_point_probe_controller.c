/* Synthetic SDK stubs only: no DJI binary, device, sockets or credentials.
 * Run each scenario in a fresh process because the controller is single-use. */
#define _POSIX_C_SOURCE 200809L
#include "l3_point_probe_controller.h"
#include "dji_camera_manager.h"
#include "dji_error.h"
#include "dji_mop_channel.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const char *scenario;
static volatile sig_atomic_t cancelled;
static atomic_uint starts, stops, readers, delivered;
static _Atomic uint64_t start_return_ns, first_stop_ns, last_stop_ns;

static uint64_t now_ns(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&delay, &delay) != 0) { }
}

T_DjiReturnCode DjiCameraManager_GetCameraType(E_DjiMountPosition mount, E_DjiCameraType *type)
{
    CHECK(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    *type = strcmp(scenario, "wrong-type") == 0 ? DJI_CAMERA_TYPE_UNKNOWN : DJI_CAMERA_TYPE_L3;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiMopChannel_Init(void)
{
    if (strcmp(scenario, "prepare-cancel") == 0) cancelled = 1;
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiCameraManager_StartRecordPointCloud(E_DjiMountPosition mount)
{
    CHECK(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    CHECK(atomic_fetch_add(&starts, 1) == 0);
    if (strcmp(scenario, "late-start") == 0) sleep_ms(300);
    atomic_store(&start_return_ns, now_ns());
    return strcmp(scenario, "start-error") == 0 ? DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT
                                                 : DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode DjiCameraManager_StopRecordPointCloud(E_DjiMountPosition mount)
{
    CHECK(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    unsigned index = atomic_fetch_add(&stops, 1);
    CHECK(index < 3);
    uint64_t now = now_ns();
    if (index == 0) atomic_store(&first_stop_ns, now);
    atomic_store(&last_stop_ns, now);
    if (strcmp(scenario, "stop-block") == 0) sleep_ms(10000);
    return strcmp(scenario, "stop-failure") == 0 ? DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT
                                                  : DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

L3PointCloudResult l3_pointcloud_psdk_read(E_DjiMountPosition mount, uint32_t attempts,
                                         uint32_t packets, L3PointCloudPacketSink sink,
                                         void *user, L3PointCloudReadReport *report)
{
    CHECK(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1 && attempts == 60 && packets == 30);
    atomic_fetch_add(&readers, 1);
    if (strcmp(scenario, "reader-block") == 0) sleep_ms(6000);
    static const uint8_t synthetic[] = "synthetic-controller-test";
    memset(report, 0, sizeof(*report));
    report->receive_attempts = 1;
    if (!sink(synthetic, sizeof(synthetic), user)) return L3_POINTCLOUD_SINK_STOPPED;
    report->packets_delivered = 1;
    return L3_POINTCLOUD_OK;
}

static bool count_packet(const uint8_t *packet, uint32_t size, void *user)
{
    (void)user;
    CHECK(size == sizeof("synthetic-controller-test"));
    CHECK(memcmp(packet, "synthetic-controller-test", size) == 0);
    atomic_fetch_add(&delivered, 1);
    return strcmp(scenario, "sink-failure") != 0;
}

L3PointCloudResult l3_pointcloud_psdk_read_until(E_DjiMountPosition mount,
                                               L3PointCloudStopRequested should_stop,
                                               L3PointCloudPacketSink sink, void *user,
                                               L3PointCloudReadReport *report)
{
    CHECK(mount == DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1);
    atomic_fetch_add(&readers, 1);
    static const uint8_t synthetic[] = "synthetic-controller-test";
    memset(report, 0, sizeof(*report));
    while (!should_stop(user)) {
        ++report->receive_attempts;
        if (!sink(synthetic, sizeof(synthetic), user)) break;
        ++report->packets_delivered;
        sleep_ms(1);
    }
    return L3_POINTCLOUD_SINK_STOPPED;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    scenario = argv[1];
    cancelled = strcmp(scenario, "pre-cancel") == 0;
    L3PointProbeConfig config = {
        .mount = DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1,
        .duration_ms = strcmp(scenario, "normal") == 0 || strcmp(scenario, "reader-block") == 0 ||
                       strcmp(scenario, "sink-failure") == 0 || strcmp(scenario, "continuous") == 0 ? 3200 : 50,
        .continuous = strcmp(scenario, "continuous") == 0,
        .confirmed_no_existing_recording = true,
        .sink = count_packet,
        .cancel_flag = &cancelled,
    };
    L3PointProbeReport report;
    L3PointProbeController *retained = NULL;
    uint64_t begin = now_ns();
    L3PointProbeOutcome outcome = l3_point_probe_run(&config, &report, &retained);
    uint64_t elapsed_ms = (now_ns() - begin) / UINT64_C(1000000);
    printf("test_event=result scenario=%s outcome=%d elapsed_ms=%llu starts=%u start_finished=%d\n",
           scenario, outcome, (unsigned long long)elapsed_ms, atomic_load(&starts), report.start_finished);
    CHECK(elapsed_ms < 8000);
    if (strcmp(scenario, "pre-cancel") == 0 || strcmp(scenario, "prepare-cancel") == 0) {
        CHECK(outcome == L3_POINT_PROBE_CANCELLED && report.cancelled && report.cleanup_safe && retained == NULL);
        CHECK(atomic_load(&starts) == 0 && atomic_load(&stops) == 0 && atomic_load(&readers) == 0);
    } else if (strcmp(scenario, "wrong-type") == 0) {
        CHECK(outcome == L3_POINT_PROBE_NOT_L3 && atomic_load(&starts) == 0 && atomic_load(&stops) == 0);
    } else {
        CHECK(atomic_load(&starts) == 1 && report.start_attempted && report.start_finished);
        CHECK(report.stop_attempts == atomic_load(&stops));
        if (strcmp(scenario, "continuous") == 0) {
            CHECK(outcome == L3_POINT_PROBE_OK && report.cleanup_safe && retained == NULL);
            CHECK(report.reader_report.packets_delivered > 30 && atomic_load(&stops) == 1);
            CHECK(elapsed_ms >= 3150 && elapsed_ms < 4000);
        } else if (strcmp(scenario, "normal") == 0) {
            CHECK(outcome == L3_POINT_PROBE_OK && report.cleanup_safe && retained == NULL);
            CHECK(atomic_load(&delivered) == 1 && atomic_load(&stops) == 1);
        } else if (strcmp(scenario, "reader-block") == 0) {
            CHECK(outcome == L3_POINT_PROBE_WORKERS_PENDING && report.stop_command_confirmed);
            CHECK(!report.reader_finished && !report.cleanup_safe && retained != NULL);
            CHECK((atomic_load(&first_stop_ns) - begin) / UINT64_C(1000000) < 4000);
            CHECK(!l3_point_probe_release_if_safe(retained));
        } else if (strcmp(scenario, "late-start") == 0) {
            CHECK(report.cleanup_safe && report.stop_command_confirmed && retained == NULL);
            CHECK(atomic_load(&stops) == 2 && atomic_load(&readers) == 0);
            CHECK(atomic_load(&first_stop_ns) < atomic_load(&start_return_ns));
            CHECK(atomic_load(&last_stop_ns) >= atomic_load(&start_return_ns));
        } else if (strcmp(scenario, "start-error") == 0) {
            CHECK(outcome == L3_POINT_PROBE_ACQUIRE_FAILED && report.cleanup_safe && retained == NULL);
            CHECK(report.start_code == DJI_ERROR_SYSTEM_MODULE_CODE_TIMEOUT);
            CHECK(report.stop_command_confirmed && atomic_load(&stops) == 1 && atomic_load(&readers) == 0);
        } else if (strcmp(scenario, "sink-failure") == 0) {
            CHECK(outcome == L3_POINT_PROBE_ACQUIRE_FAILED && report.cleanup_safe && retained == NULL);
            CHECK(report.stop_command_confirmed && report.sink_failed);
            CHECK(atomic_load(&delivered) == 1 && atomic_load(&stops) == 1);
        } else if (strcmp(scenario, "stop-failure") == 0) {
            CHECK(outcome == L3_POINT_PROBE_STOP_UNCONFIRMED && !report.cleanup_safe && retained != NULL);
            CHECK(report.stop_finished && !report.stop_command_confirmed && atomic_load(&stops) == 3);
            CHECK(!l3_point_probe_release_if_safe(retained));
        } else if (strcmp(scenario, "stop-block") == 0) {
            CHECK(outcome == L3_POINT_PROBE_STOP_UNCONFIRMED && !report.cleanup_safe && retained != NULL);
            CHECK(!report.stop_finished && !report.stop_command_confirmed && elapsed_ms < 4000);
            CHECK(!l3_point_probe_release_if_safe(retained));
        } else { CHECK(!"unknown scenario"); }
    }
    printf("PASS %s outcome=%d elapsed_ms=%llu start=%u stop=%u reader=%u confirmed=%d cleanup=%d\n",
           scenario, outcome, (unsigned long long)elapsed_ms, atomic_load(&starts), atomic_load(&stops),
           atomic_load(&readers), report.stop_command_confirmed, report.cleanup_safe);
    /* Pending workers exist only in mocks. Keep all context alive until the
     * process exits, exactly as required by the controller's pending contract. */
    return 0;
}
