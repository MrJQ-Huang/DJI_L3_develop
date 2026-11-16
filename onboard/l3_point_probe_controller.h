#ifndef L3_POINT_PROBE_CONTROLLER_H
#define L3_POINT_PROBE_CONTROLLER_H

#include <signal.h>
#include "l3_pointcloud_psdk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct L3PointProbeController L3PointProbeController;

typedef enum {
    L3_POINT_PROBE_OK = 0,
    L3_POINT_PROBE_CANCELLED,
    L3_POINT_PROBE_INVALID_ARGUMENT,
    L3_POINT_PROBE_NOT_L3,
    L3_POINT_PROBE_PREPARE_FAILED,
    L3_POINT_PROBE_THREAD_FAILED,
    L3_POINT_PROBE_STOP_UNCONFIRMED,
    L3_POINT_PROBE_WORKERS_PENDING,
    L3_POINT_PROBE_ACQUIRE_FAILED
} L3PointProbeOutcome;

typedef struct {
    E_DjiMountPosition mount; /* Actual L3 payload mount, only ports 1..3. */
    uint32_t duration_ms;     /* 1..20000; counted from before the sole Start. */
    /* The public SDK has no documented point-cloud-specific recording-state
     * getter. This must represent the user's current confirmation of no
     * existing recording and no other recording control during this session. */
    bool confirmed_no_existing_recording;
    L3PointCloudPacketSink sink;
    void *sink_context;
    /* Optional signal flag, read only by the thread running this function. */
    const volatile sig_atomic_t *cancel_flag;
} L3PointProbeConfig;

typedef struct {
    bool start_attempted;
    bool start_finished;
    bool reader_started;
    bool reader_finished;
    bool stop_requested;
    bool stop_finished;
    bool stop_command_confirmed;
    bool cleanup_safe;
    bool sink_failed;
    bool clock_failed;
    bool cancelled;
    uint32_t stop_attempts;
    T_DjiReturnCode camera_type_code;
    T_DjiReturnCode mop_init_code;
    T_DjiReturnCode start_code;
    T_DjiReturnCode stop_code; /* Last Stop; use attempts/confirmed flags too. */
    L3PointCloudResult reader_result;
    L3PointCloudReadReport reader_report;
    uint64_t start_requested_monotonic_ns;
    uint64_t deadline_monotonic_ns;
    uint64_t last_stop_requested_monotonic_ns;
    uint64_t stop_ack_monotonic_ns;
} L3PointProbeReport;

/* Caller owns initialized Core/CameraManager and exclusive PSDK use. One
 * invocation per process. Starts L3 point-cloud recording ONCE, then reads at
 * most 30 valid packets / 60 attempts through the existing read-only adapter.
 * The reader and stop timer run on separate threads. No video/source/flight
 * operation is performed here.
 *
 * The timer requests Stop at duration_ms, even if MOP blocks or returns zero
 * packets. Start/Stop/SDK cleanup themselves have no documented time limit;
 * this is NOT a hard guarantee of a 20-second physical recording. If Start
 * returns late, another Stop is sent after its return. Total Stop calls <= 3.
 * A successful Stop issued after Start returned sets stop_command_confirmed;
 * it is not an independently measured sensor-state confirmation.
 *
 * After arming Start, the caller waits no later than its deadline + 3 seconds
 * for workers (subject to normal OS scheduling and a nonblocking sink). If
 * any worker is pending or Stop is unconfirmed, returns a retained heap
 * controller. DO NOT free sink_context/cancel_flag or deinitialize shared SDK
 * while it is retained. Do not pthread_cancel blocked SDK threads. Immediately
 * report a missing Stop ACK and arrange manual L3 stop in Pilot; killing this
 * process does not itself stop recording. A second simultaneous PSDK process
 * is not a safe substitute for this timer.
 *
 * Calls before Start (type check/MOP Init) are blocking SDK operations, too;
 * an external watchdog is required for the entire command. */
L3PointProbeOutcome l3_point_probe_run(const L3PointProbeConfig *config,
                                      L3PointProbeReport *report,
                                      L3PointProbeController **retained);

/* Normal-thread only, not async-signal-safe. Does not call the SDK. */
void l3_point_probe_request_stop(L3PointProbeController *controller);
void l3_point_probe_snapshot(L3PointProbeController *controller, L3PointProbeReport *report);

/* Reclaims only after all workers completed AND either no Start was attempted
 * or its final Stop was acknowledged. Single control-thread owner; never race
 * release with snapshot/request_stop. False means caller must retain it. */
bool l3_point_probe_release_if_safe(L3PointProbeController *controller);

#ifdef __cplusplus
}
#endif
#endif
