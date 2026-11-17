#ifndef L3_USB_PADDING_H
#define L3_USB_PADDING_H
#include <stddef.h>
#include <stdint.h>

/* Used ONLY by the pinned SDK object's corrected USB send call site.
 * The call supplies the original uint16 payload length, before USB padding.
 * Destination capacity is provided by the SDK's 61440-byte staging buffer. */
void *L3Usb_CopyPadded(void *destination, const void *source, size_t length);
uint64_t l3_usb_padding_corrections(void);
#endif
