#include "l3_usb_padding.h"
#include <stdatomic.h>
#include <string.h>

static atomic_uint_fast64_t corrections;

void *L3Usb_CopyPadded(void *destination, const void *source, size_t length)
{
    if (length == 0) return destination;
    memcpy(destination, source, length);
    if (length < 4096) {
        /* The old SDK copies 4096 from a possibly 28-byte source. Copy only
         * the payload; explicitly initialize the USB padding at destination.
         * Allocation sizes, ASan redzones and SDK shutdown remain unchanged. */
        memset((unsigned char *)destination + length, 0, 4096 - length);
        atomic_fetch_add_explicit(&corrections, 1, memory_order_relaxed);
    }
    return destination;
}

uint64_t l3_usb_padding_corrections(void)
{
    return atomic_load_explicit(&corrections, memory_order_relaxed);
}
