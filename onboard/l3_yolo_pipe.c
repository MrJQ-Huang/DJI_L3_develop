#include "l3_yolo_pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { INPUT_LIMIT = 4 * 1024 * 1024, WRITE_BATCH = 128 * 1024 };

struct L3YoloPipe {
    int fd;
    pid_t pid;
    pthread_mutex_t lock;
    size_t head;
    size_t queued;
    bool overflow;
    uint8_t input[INPUT_LIMIT];
};

extern char **environ;

L3YoloPipe *l3_yolo_pipe_start(const char *worker, const char *engine, unsigned int bitrate)
{
    if (!worker || !engine || !bitrate) return NULL;
    char rate[16];
    snprintf(rate, sizeof(rate), "%u", bitrate);
    char *args[] = {(char *)worker, "--engine", (char *)engine, "--bitrate", rate, NULL};
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) return NULL;
    L3YoloPipe *pipe = calloc(1, sizeof(*pipe));
    if (!pipe) {
        close(pair[0]);
        close(pair[1]);
        return NULL;
    }
    int error = pthread_mutex_init(&pipe->lock, NULL);
    if (error) {
        close(pair[0]);
        close(pair[1]);
        free(pipe);
        errno = error;
        return NULL;
    }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
        /* The launcher intentionally inherits descriptor 9 for its SDK flock.
         * The worker is subordinate, so it must not own the SDK session lock. */
        if (!error && fcntl(9, F_GETFD) != -1) error = posix_spawn_file_actions_addclose(&actions, 9);
        if (!error) error = posix_spawn(&pipe->pid, worker, &actions, NULL, args, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pair[1]);
    if (error) {
        close(pair[0]);
        pthread_mutex_destroy(&pipe->lock);
        free(pipe);
        errno = error;
        return NULL;
    }
    pipe->fd = pair[0];
    return pipe;
}

bool l3_yolo_pipe_enqueue(L3YoloPipe *pipe, const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&pipe->lock);
    if (size > INPUT_LIMIT - pipe->queued) pipe->overflow = true;
    bool ok = !pipe->overflow;
    if (ok) {
        size_t tail = (pipe->head + pipe->queued) % INPUT_LIMIT;
        size_t first = size < INPUT_LIMIT - tail ? size : INPUT_LIMIT - tail;
        memcpy(pipe->input + tail, data, first);
        memcpy(pipe->input, data + first, size - first);
        pipe->queued += size;
    }
    pthread_mutex_unlock(&pipe->lock);
    return ok;
}

ssize_t l3_yolo_pipe_read(L3YoloPipe *pipe, uint8_t *buffer, size_t capacity)
{
    ssize_t count = recv(pipe->fd, buffer, capacity, MSG_DONTWAIT);
    if (count > 0) return count;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

int l3_yolo_pipe_ready(L3YoloPipe *pipe)
{
    uint8_t token = 0;
    ssize_t count = l3_yolo_pipe_read(pipe, &token, 1);
    if (count <= 0) return (int)count;
    return token == 'R' ? 1 : -1;
}

bool l3_yolo_pipe_flush(L3YoloPipe *pipe)
{
    pthread_mutex_lock(&pipe->lock);
    bool ok = !pipe->overflow;
    if (ok && pipe->queued) {
        size_t length = pipe->queued < WRITE_BATCH ? pipe->queued : WRITE_BATCH;
        if (length > INPUT_LIMIT - pipe->head) length = INPUT_LIMIT - pipe->head;
        ssize_t sent = send(pipe->fd, pipe->input + pipe->head, length, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            pipe->head = (pipe->head + (size_t)sent) % INPUT_LIMIT;
            pipe->queued -= (size_t)sent;
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            ok = false;
        }
    }
    pthread_mutex_unlock(&pipe->lock);
    return ok;
}

void l3_yolo_pipe_stop(L3YoloPipe *pipe)
{
    if (!pipe) return;
    close(pipe->fd);
    pid_t result;
    do result = waitpid(pipe->pid, NULL, WNOHANG); while (result < 0 && errno == EINTR);
    if (result == 0) {
        kill(pipe->pid, SIGTERM);
        for (int retry = 0; retry < 20 && result == 0; ++retry) {
            struct timespec pause = {.tv_nsec = 50 * 1000 * 1000};
            nanosleep(&pause, NULL);
            do result = waitpid(pipe->pid, NULL, WNOHANG); while (result < 0 && errno == EINTR);
        }
        if (result == 0) {
            kill(pipe->pid, SIGKILL);
            do result = waitpid(pipe->pid, NULL, 0); while (result < 0 && errno == EINTR);
        }
    }
    pthread_mutex_destroy(&pipe->lock);
    free(pipe);
}
