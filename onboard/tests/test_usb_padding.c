#include "l3_usb_padding.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    const size_t lengths[] = {1, 28, 511, 4095, 4096, 4097, 61440};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        size_t n = lengths[i], capacity = n < 4096 ? 4096 : n;
        /* ASan sees exactly n source bytes. The old 28 -> 4096 read fails here. */
        unsigned char *source = malloc(n), *destination = malloc(capacity + 32);
        assert(source && destination);
        memset(source, 0x5a, n);
        memset(destination, 0xa5, capacity + 32);
        assert(L3Usb_CopyPadded(destination, source, n) == destination);
        assert(memcmp(destination, source, n) == 0);
        for (size_t j = n; j < capacity; ++j) assert(destination[j] == 0);
        for (size_t j = capacity; j < capacity + 32; ++j) assert(destination[j] == 0xa5);
        free(source); free(destination);
    }
    assert(L3Usb_CopyPadded(NULL, NULL, 0) == NULL);
    assert(l3_usb_padding_corrections() == 4);
    puts("PASS: exact-size source, zero USB padding, unchanged payload and destination guard");
}
