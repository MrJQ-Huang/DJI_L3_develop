#ifndef L3_VIDEO_CONTROL_H
#define L3_VIDEO_CONTROL_H
#include "dji_typedef.h"
#include <stdbool.h>

typedef struct {
    int previous_source;
    int current_source; /* -1 means unknown, never infer it from the requested ID. */
    bool write_attempted;
    bool confirmed;
    T_DjiReturnCode code;
} L3VideoControlResult;

/* Only camera preview source control. No Liveview, encoding, recording or
 * flight calls. Revalidate exact L3 and its advertised source range each time. */
L3VideoControlResult l3_video_source_read(E_DjiMountPosition mount);
L3VideoControlResult l3_video_source_set(E_DjiMountPosition mount, unsigned int source);
#endif
