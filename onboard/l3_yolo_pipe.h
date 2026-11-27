#ifndef L3_YOLO_PIPE_H
#define L3_YOLO_PIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct L3YoloPipe L3YoloPipe;

L3YoloPipe *l3_yolo_pipe_start(const char *worker, const char *engine, unsigned int bitrate);
/* Borrowed PSDK bytes are copied into a bounded queue, without socket I/O. */
bool l3_yolo_pipe_enqueue(L3YoloPipe *pipe, const uint8_t *data, size_t size);
/* Control thread only. Return -1 on failure, 0 while starting, 1 once ready. */
int l3_yolo_pipe_ready(L3YoloPipe *pipe);
bool l3_yolo_pipe_flush(L3YoloPipe *pipe);
/* Return -1 on failure, 0 if the output socket has no data. */
ssize_t l3_yolo_pipe_read(L3YoloPipe *pipe, uint8_t *buffer, size_t capacity);
/* Stop only after the caller has drained the PSDK callbacks. */
void l3_yolo_pipe_stop(L3YoloPipe *pipe);

#endif
