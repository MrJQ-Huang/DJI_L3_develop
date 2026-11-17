#define _POSIX_C_SOURCE 200809L
#include "l3_point_probe_controller.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dji_camera_manager.h"
#include "dji_error.h"
#include "dji_mop_channel.h"

enum { STOP_CALL_LIMIT = 3, STOP_RETRY_MS = 250, SETTLE_MS = 3000,
       CONTROL_POLL_MS = 50, RETURN_GRACE_MS = 3000 };

struct L3PointProbeController {
    pthread_mutex_t lock;
    pthread_cond_t changed; /* CLOCK_MONOTONIC */
    pthread_t starter, reader, stopper;
    bool starter_created, reader_created, stopper_created;
    bool launch, abort_before_launch, stopper_ready;
    L3PointProbeConfig config;
    L3PointProbeReport report;
    struct timespec t0, deadline;
    struct timespec first_stop_request;
    bool first_stop_request_valid;
};

static pthread_mutex_t invocation_lock = PTHREAD_MUTEX_INITIALIZER;
static bool invoked;

static struct timespec add_ms(struct timespec time, uint32_t milliseconds)
{
    time.tv_sec += (time_t)(milliseconds / 1000);
    time.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (time.tv_nsec >= 1000000000L) {
        ++time.tv_sec;
        time.tv_nsec -= 1000000000L;
    }
    return time;
}

static bool before(struct timespec a, struct timespec b)
{
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

static uint64_t ns_of(struct timespec value)
{
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

/* All helpers ending in _locked require lock. Never call SDK or user sink
 * while holding it; a blocked receive must not prevent the stop timer. */
static bool now_locked(L3PointProbeController *c, struct timespec *now)
{
    if (clock_gettime(CLOCK_MONOTONIC, now) == 0) return true;
    c->report.clock_failed = true;
    c->report.stop_requested = true;
    pthread_cond_broadcast(&c->changed);
    return false;
}

static void request_locked(L3PointProbeController *c)
{
    if (!c->report.stop_requested &&
        clock_gettime(CLOCK_MONOTONIC, &c->first_stop_request) == 0) c->first_stop_request_valid = true;
    c->report.stop_requested = true;
    pthread_cond_broadcast(&c->changed);
}

static bool workers_finished_locked(const L3PointProbeController *c)
{
    return c->report.start_finished && c->report.reader_finished && c->report.stop_finished;
}

static bool cleanup_safe_locked(const L3PointProbeController *c)
{
    return workers_finished_locked(c) &&
           (!c->report.start_attempted || c->report.stop_command_confirmed);
}

static void copy_report_locked(L3PointProbeController *c, L3PointProbeReport *report)
{
    c->report.cleanup_safe = cleanup_safe_locked(c);
    *report = c->report;
}

static bool await_launch_locked(L3PointProbeController *c)
{
    while (!c->launch && !c->abort_before_launch) pthread_cond_wait(&c->changed, &c->lock);
    return !c->abort_before_launch;
}

static void *start_worker(void *opaque)
{
    L3PointProbeController *c = opaque;
    pthread_mutex_lock(&c->lock);
    if (!await_launch_locked(c)) goto finished;
    struct timespec now;
    if (!now_locked(c, &now) || !before(now, c->deadline) || c->report.stop_requested) {
        request_locked(c);
        goto finished;
    }
    c->report.start_attempted = true;
    pthread_mutex_unlock(&c->lock);

    T_DjiReturnCode result = DjiCameraManager_StartRecordPointCloud(c->config.mount);

    pthread_mutex_lock(&c->lock);
    c->report.start_code = result;
    if (result != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) request_locked(c);
finished:
    c->report.start_finished = true;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

static bool receive_sink(const uint8_t *packet, uint32_t size, void *opaque)
{
    L3PointProbeController *c = opaque;
    pthread_mutex_lock(&c->lock);
    struct timespec now;
    bool deliver = now_locked(c, &now) && before(now, c->deadline) && !c->report.stop_requested;
    if (!deliver) request_locked(c);
    pthread_mutex_unlock(&c->lock);
    if (!deliver) return false;

    bool result = c->config.sink(packet, size, c->config.sink_context);
    if (!result) {
        pthread_mutex_lock(&c->lock);
        c->report.sink_failed = true;
        request_locked(c);
        pthread_mutex_unlock(&c->lock);
    }
    return result;
}

static bool reader_should_stop(void *opaque)
{
    L3PointProbeController *c = opaque;
    pthread_mutex_lock(&c->lock);
    struct timespec now;
    if (!now_locked(c, &now) || !before(now, c->deadline)) request_locked(c);
    bool stop = c->report.stop_requested;
    pthread_mutex_unlock(&c->lock);
    return stop;
}

static void *reader_worker(void *opaque)
{
    L3PointProbeController *c = opaque;
    pthread_mutex_lock(&c->lock);
    if (!await_launch_locked(c)) goto finished;
    while (!c->report.start_finished) pthread_cond_wait(&c->changed, &c->lock);
    if (!c->report.start_attempted || c->report.start_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS ||
        c->report.stop_requested) goto finished;

    /* Follow the official example's preparation delay without blocking Stop. */
    struct timespec settle = add_ms(c->t0, SETTLE_MS);
    if (before(c->deadline, settle)) settle = c->deadline;
    struct timespec now;
    while (!c->report.stop_requested) {
        if (!now_locked(c, &now) || !before(now, settle)) break;
        int error = pthread_cond_timedwait(&c->changed, &c->lock, &settle);
        if (error != 0 && error != ETIMEDOUT) { request_locked(c); break; }
    }
    if (!now_locked(c, &now) || !before(now, c->deadline) || c->report.stop_requested) goto finished;
    c->report.reader_started = true;
    pthread_mutex_unlock(&c->lock);

    L3PointCloudReadReport reader_report = {0};
    L3PointCloudResult result = c->config.continuous
        ? l3_pointcloud_psdk_read_until(c->config.mount, reader_should_stop,
                                       receive_sink, c, &reader_report)
        : l3_pointcloud_psdk_read(c->config.mount, 60, 30, receive_sink, c, &reader_report);

    pthread_mutex_lock(&c->lock);
    c->report.reader_report = reader_report;
    c->report.reader_result = result;
    request_locked(c); /* End recording early after a limit, error, or sink stop. */
finished:
    c->report.reader_finished = true;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

static void call_stop_locked(L3PointProbeController *c, bool after_start_finished)
{
    struct timespec now;
    if (now_locked(c, &now)) c->report.last_stop_requested_monotonic_ns = ns_of(now);
    ++c->report.stop_attempts;
    pthread_mutex_unlock(&c->lock);

    T_DjiReturnCode result = DjiCameraManager_StopRecordPointCloud(c->config.mount);

    pthread_mutex_lock(&c->lock);
    c->report.stop_code = result;
    if (result == DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS && after_start_finished) {
        c->report.stop_command_confirmed = true;
        if (now_locked(c, &now)) c->report.stop_ack_monotonic_ns = ns_of(now);
    }
    pthread_cond_broadcast(&c->changed);
}

static void wait_retry_locked(L3PointProbeController *c)
{
    struct timespec now;
    if (!now_locked(c, &now)) return;
    struct timespec retry = add_ms(now, STOP_RETRY_MS);
    while (now_locked(c, &now) && before(now, retry)) {
        int error = pthread_cond_timedwait(&c->changed, &c->lock, &retry);
        if (error != 0 && error != ETIMEDOUT) break;
    }
}

static void *stop_worker(void *opaque)
{
    L3PointProbeController *c = opaque;
    pthread_mutex_lock(&c->lock);
    c->stopper_ready = true;
    pthread_cond_broadcast(&c->changed);
    if (!await_launch_locked(c)) goto finished;
    while (!c->report.stop_requested) {
        struct timespec now;
        if (!now_locked(c, &now) || !before(now, c->deadline)) break;
        int error = pthread_cond_timedwait(&c->changed, &c->lock, &c->deadline);
        if (error != 0 && error != ETIMEDOUT) break;
    }
    request_locked(c);

    /* A Stop issued while Start is pending can race a late Start. Do not treat
     * its ACK as final and do not spend retries until Start has returned. */
    if (c->report.start_attempted && !c->report.start_finished) call_stop_locked(c, false);
    while (!c->report.start_finished) pthread_cond_wait(&c->changed, &c->lock);
    if (!c->report.start_attempted) goto finished;

    do {
        call_stop_locked(c, true);
        if (c->report.stop_command_confirmed) break;
        if (c->report.stop_attempts < STOP_CALL_LIMIT) wait_retry_locked(c);
    } while (c->report.stop_attempts < STOP_CALL_LIMIT);
finished:
    c->report.stop_finished = true;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

void l3_point_probe_request_stop(L3PointProbeController *c)
{
    if (c == NULL) return;
    pthread_mutex_lock(&c->lock);
    c->report.cancelled = true;
    request_locked(c);
    pthread_mutex_unlock(&c->lock);
}

void l3_point_probe_snapshot(L3PointProbeController *c, L3PointProbeReport *report)
{
    if (c == NULL || report == NULL) return;
    pthread_mutex_lock(&c->lock);
    copy_report_locked(c, report);
    pthread_mutex_unlock(&c->lock);
}

bool l3_point_probe_release_if_safe(L3PointProbeController *c)
{
    if (c == NULL) return true;
    pthread_mutex_lock(&c->lock);
    bool safe = cleanup_safe_locked(c);
    pthread_mutex_unlock(&c->lock);
    if (!safe) return false;
    if (c->starter_created && pthread_join(c->starter, NULL) != 0) return false;
    c->starter_created = false;
    if (c->reader_created && pthread_join(c->reader, NULL) != 0) return false;
    c->reader_created = false;
    if (c->stopper_created && pthread_join(c->stopper, NULL) != 0) return false;
    c->stopper_created = false;
    pthread_cond_destroy(&c->changed);
    pthread_mutex_destroy(&c->lock);
    free(c);
    return true;
}

static L3PointProbeOutcome outcome_of(const L3PointProbeReport *r)
{
    if (r->start_attempted && !r->stop_command_confirmed) return L3_POINT_PROBE_STOP_UNCONFIRMED;
    if (!r->cleanup_safe) return L3_POINT_PROBE_WORKERS_PENDING;
    if (r->cancelled) return L3_POINT_PROBE_CANCELLED;
    if (!r->start_attempted || r->start_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS || r->clock_failed ||
        r->sink_failed || !r->reader_started || r->reader_report.packets_delivered == 0 ||
        r->reader_result > L3_POINTCLOUD_READ_LIMIT) return L3_POINT_PROBE_ACQUIRE_FAILED;
    return L3_POINT_PROBE_OK;
}

L3PointProbeOutcome l3_point_probe_run(const L3PointProbeConfig *config,
                                      L3PointProbeReport *report,
                                      L3PointProbeController **retained)
{
    if (report == NULL || retained == NULL) return L3_POINT_PROBE_INVALID_ARGUMENT;
    memset(report, 0, sizeof(*report));
    *retained = NULL;
    report->cleanup_safe = true;
    if (config == NULL || config->sink == NULL || !config->confirmed_no_existing_recording ||
        config->duration_ms == 0 || config->duration_ms > (config->continuous ? 300000U : 20000U) ||
        (config->mount != DJI_MOUNT_POSITION_PAYLOAD_PORT_NO1 &&
         config->mount != DJI_MOUNT_POSITION_PAYLOAD_PORT_NO2 &&
         config->mount != DJI_MOUNT_POSITION_PAYLOAD_PORT_NO3)) return L3_POINT_PROBE_INVALID_ARGUMENT;

    pthread_mutex_lock(&invocation_lock);
    bool already_invoked = invoked;
    invoked = true;
    pthread_mutex_unlock(&invocation_lock);
    if (already_invoked) return L3_POINT_PROBE_INVALID_ARGUMENT;
    if (config->cancel_flag != NULL && *config->cancel_flag) {
        report->cancelled = true;
        return L3_POINT_PROBE_CANCELLED;
    }

    E_DjiCameraType type = DJI_CAMERA_TYPE_UNKNOWN;
    report->camera_type_code = DjiCameraManager_GetCameraType(config->mount, &type);
    if (report->camera_type_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) return L3_POINT_PROBE_PREPARE_FAILED;
    if (type != DJI_CAMERA_TYPE_L3) return L3_POINT_PROBE_NOT_L3;
    if (config->cancel_flag != NULL && *config->cancel_flag) {
        report->cancelled = true;
        return L3_POINT_PROBE_CANCELLED;
    }
    report->mop_init_code = DjiMopChannel_Init();
    if (report->mop_init_code != DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS) return L3_POINT_PROBE_PREPARE_FAILED;
    if (config->cancel_flag != NULL && *config->cancel_flag) {
        report->cancelled = true;
        return L3_POINT_PROBE_CANCELLED;
    }

    L3PointProbeController *c = calloc(1, sizeof(*c));
    if (c == NULL) return L3_POINT_PROBE_PREPARE_FAILED;
    c->config = *config;
    c->report = *report;
    c->report.reader_result = L3_POINTCLOUD_INVALID_ARGUMENT;
    if (pthread_mutex_init(&c->lock, NULL) != 0) { free(c); return L3_POINT_PROBE_THREAD_FAILED; }
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0) {
        pthread_mutex_destroy(&c->lock); free(c); return L3_POINT_PROBE_THREAD_FAILED;
    }
    int error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0) error = pthread_cond_init(&c->changed, &attributes);
    pthread_condattr_destroy(&attributes);
    if (error != 0) { pthread_mutex_destroy(&c->lock); free(c); return L3_POINT_PROBE_THREAD_FAILED; }

    c->reader_created = pthread_create(&c->reader, NULL, reader_worker, c) == 0;
    if (c->reader_created) c->stopper_created = pthread_create(&c->stopper, NULL, stop_worker, c) == 0;
    if (c->stopper_created) c->starter_created = pthread_create(&c->starter, NULL, start_worker, c) == 0;
    pthread_mutex_lock(&c->lock);
    /* Starter/reader are gated by launch. Confirm that the independent timer
     * thread has really run before allowing the single Start SDK call. */
    while (c->starter_created && !c->stopper_ready) pthread_cond_wait(&c->changed, &c->lock);
    bool cancelled_before_launch = config->cancel_flag != NULL && *config->cancel_flag;
    if (!c->starter_created || cancelled_before_launch || !now_locked(c, &c->t0)) {
        if (cancelled_before_launch) c->report.cancelled = true;
        c->abort_before_launch = true;
        if (!c->starter_created) c->report.start_finished = true;
        if (!c->reader_created) c->report.reader_finished = true;
        if (!c->stopper_created) c->report.stop_finished = true;
        pthread_cond_broadcast(&c->changed);
        pthread_mutex_unlock(&c->lock);
        /* No SDK calls are made by workers before launch, so these joins cannot
         * depend on aircraft I/O or a sink. */
        if (c->starter_created) pthread_join(c->starter, NULL);
        if (c->reader_created) pthread_join(c->reader, NULL);
        if (c->stopper_created) pthread_join(c->stopper, NULL);
        c->starter_created = c->reader_created = c->stopper_created = false;
        l3_point_probe_snapshot(c, report);
        l3_point_probe_release_if_safe(c);
        return cancelled_before_launch ? L3_POINT_PROBE_CANCELLED : L3_POINT_PROBE_THREAD_FAILED;
    }
    c->deadline = add_ms(c->t0, config->duration_ms);
    c->report.start_requested_monotonic_ns = ns_of(c->t0);
    c->report.deadline_monotonic_ns = ns_of(c->deadline);
    c->launch = true;
    pthread_cond_broadcast(&c->changed);
    struct timespec return_deadline = add_ms(c->deadline, RETURN_GRACE_MS);
    while (!workers_finished_locked(c)) {
        if (config->cancel_flag != NULL && *config->cancel_flag) {
            c->report.cancelled = true;
            request_locked(c);
        }
        struct timespec now;
        if (c->first_stop_request_valid) {
            struct timespec early_return = add_ms(c->first_stop_request, RETURN_GRACE_MS);
            if (before(early_return, return_deadline)) return_deadline = early_return;
        }
        if (!now_locked(c, &now) || !before(now, return_deadline)) break;
        struct timespec wake = add_ms(now, CONTROL_POLL_MS);
        if (before(return_deadline, wake)) wake = return_deadline;
        error = pthread_cond_timedwait(&c->changed, &c->lock, &wake);
        if (error != 0 && error != ETIMEDOUT) { request_locked(c); break; }
    }
    copy_report_locked(c, report);
    pthread_mutex_unlock(&c->lock);
    L3PointProbeOutcome outcome = outcome_of(report);
    if (!l3_point_probe_release_if_safe(c)) *retained = c;
    return outcome;
}
