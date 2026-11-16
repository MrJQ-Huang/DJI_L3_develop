#ifndef L3_RGB_PROBE_H
#define L3_RGB_PROBE_H

#include "dji_typedef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed encoded bytes, valid only during this call. A callback is not a
 * frame boundary. Copy to a bounded queue and return promptly; do not decode,
 * block on I/O, or call either lifecycle function from the sink. The sink must
 * be thread-safe and may be called before start returns. */
typedef void (*L3RgbProbeSink)(const uint8_t *data, uint32_t size, void *user);

/* PSDK core, Camera Manager and Liveview must already be initialized by the
 * caller, with no other Liveview subscription on this camera. Pass the actual
 * payload mount, not FPV. Only the documented Liveview positions 1-3 qualify;
 * the exact camera type must be L3. No source selection or recording commands
 * are issued. DEFAULT is unverified on L3: decode and check for visible-light
 * content before treating this as an RGB stream.
 *
 * Run start/stop from one control thread, and apply a finite probe deadline
 * there. Only one SDK start attempt is permitted per process because the SDK
 * callback carries no session token. After any SDK start attempt call stop,
 * including after a start error, before deinitializing shared SDK modules.
 * Keep user/sink valid until start fails or stop returns. */
T_DjiReturnCode l3_rgb_probe_start(E_DjiMountPosition mount,
                                   L3RgbProbeSink sink, void *user);

/* Disable delivery and drain in-flight sinks, even if the SDK reports a stop
 * error. A failed SDK stop may be retried; it does not prove the subscription
 * stopped. Repeating a successful stop is a no-op. */
T_DjiReturnCode l3_rgb_probe_stop(void);

#ifdef __cplusplus
}
#endif

#endif
