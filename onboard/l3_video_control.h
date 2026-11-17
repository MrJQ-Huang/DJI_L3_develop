#ifndef L3_VIDEO_CONTROL_H
#define L3_VIDEO_CONTROL_H
#include "dji_typedef.h"
#include <stdbool.h>

enum {
    L3_VIDEO_SOURCE_VISIBLE = 1, /* L3 WIDE_CAM, not VISIBLE_CAM (7). */
    L3_VIDEO_SOURCE_POINT_CLOUD = 14
};

typedef struct {
    int previous_source;
    int current_source; /* -1 means unknown, never infer it from the requested ID. */
    bool write_attempted;
    bool confirmed;
    T_DjiReturnCode code;
} L3VideoControlResult;

/* Only camera preview source control. No Liveview, encoding, recording or
 * flight calls. Revalidate exact L3 and the tested camera source IDs each time.
 * PSDK 3.18's static L3 range [7,14] is incorrect; actual sources are [1,14]. */
L3VideoControlResult l3_video_source_read(E_DjiMountPosition mount);
L3VideoControlResult l3_video_source_set(E_DjiMountPosition mount, unsigned int source);
#endif
