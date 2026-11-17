#define _POSIX_C_SOURCE 200809L
#include "l3_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { CLIENTS = 4, QUEUE_RECORDS = 128, POINT_LIMIT = 512 * 1024,
       NAL_LIMIT = 1024 * 1024, PARAMETER_LIMIT = 64 * 1024 };

typedef struct Record {
    struct Record *next;
    size_t size;
    uint8_t bytes[];
} Record;

typedef struct {
    struct L3Stream *owner;
    pthread_t thread;
    pthread_cond_t available;
    atomic_bool stopping;
    bool active, joinable, video, ready;
    int fd;
    Record *head, *tail;
    size_t bytes, records, limit;
} Client;

struct L3Stream {
    pthread_mutex_t lock;
    pthread_t acceptor;
    atomic_bool stopping;
    int listeners[2];
    Client clients[CLIENTS];
    uint32_t timeout_ms;
    L3StreamStats stats;
    uint8_t nal[NAL_LIMIT];
    size_t nal_size, zeros;
    bool have_start, video_failed;
    uint8_t *sps, *pps;
    size_t sps_size, pps_size;
};

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return -1;
    return 0;
}

static int listener(const char *ip, uint16_t requested, uint16_t *actual)
{
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(requested) };
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) { errno = EINVAL; return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    socklen_t size = sizeof(address);
    if (nonblocking(fd) != 0 || bind(fd, (struct sockaddr *)&address, size) != 0 ||
        listen(fd, 4) != 0 || getsockname(fd, (struct sockaddr *)&address, &size) != 0) {
        int error = errno;
        close(fd); errno = error; return -1;
    }
    *actual = ntohs(address.sin_port);
    return fd;
}

/* The writer owns its current record. bytes/records also count that in-flight
 * record, so queue caps include all heap payload retained for this client. */
static void clear_pending(Client *client)
{
    while (client->head != NULL) {
        Record *record = client->head;
        client->head = record->next;
        client->bytes -= record->size;
        --client->records;
        free(record);
    }
    client->tail = NULL;
}

static void disconnect_slow(Client *client)
{
    if (!atomic_exchange(&client->stopping, true)) ++client->owner->stats.slow_disconnects;
    clear_pending(client);
    pthread_cond_signal(&client->available);
}

static bool queue(Client *client, const uint8_t *bytes, size_t size)
{
    if (size > client->limit - client->bytes || client->records == QUEUE_RECORDS) return false;
    Record *record = malloc(sizeof(*record) + size);
    if (record == NULL) return false;
    record->next = NULL;
    record->size = size;
    memcpy(record->bytes, bytes, size);
    if (client->tail != NULL) client->tail->next = record;
    else client->head = record;
    client->tail = record;
    client->bytes += size;
    ++client->records;
    size_t *high = client->video ? &client->owner->stats.video_queue_high_water :
                                &client->owner->stats.point_queue_high_water;
    if (client->bytes > *high) *high = client->bytes;
    pthread_cond_signal(&client->available);
    return true;
}

static int send_record(Client *client, const Record *record)
{
    size_t offset = 0;
    int64_t start = now_ms();
    if (start < 0) return EIO;
    int64_t deadline = start + client->owner->timeout_ms;
    while (offset < record->size && !atomic_load(&client->stopping)) {
        int64_t now = now_ms();
        if (now < 0) return EIO;
        if (now >= deadline) return ETIMEDOUT;
        ssize_t sent = send(client->fd, record->bytes + offset, record->size - offset,
                            MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) { offset += (size_t)sent; continue; }
        if (sent == 0) return EPIPE;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return errno;
        int remaining = (int)(deadline - now);
        struct pollfd wait = { .fd = client->fd, .events = POLLOUT };
        int rc = poll(&wait, 1, remaining > 100 ? 100 : remaining);
        if (rc < 0 && errno != EINTR) return errno;
        if (rc > 0 && (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) return EPIPE;
    }
    return offset == record->size ? 0 : ECANCELED;
}

static void *writer(void *argument)
{
    Client *client = argument;
    L3Stream *stream = client->owner;
    pthread_mutex_lock(&stream->lock);
    for (;;) {
        while (client->head == NULL && !atomic_load(&client->stopping)) {
            struct timespec wake;
            if (clock_gettime(CLOCK_REALTIME, &wake) != 0) {
                atomic_store(&client->stopping, true);
                break;
            }
            wake.tv_nsec += 100 * 1000 * 1000;
            if (wake.tv_nsec >= 1000 * 1000 * 1000) {
                ++wake.tv_sec;
                wake.tv_nsec -= 1000 * 1000 * 1000;
            }
            if (pthread_cond_timedwait(&client->available, &stream->lock, &wake) == ETIMEDOUT) {
                /* Notice a subscriber that closes before source data arrives.
                 * Clients only receive, so input or EOF terminates the stream.
                 * No network operation runs on the publisher. */
                pthread_mutex_unlock(&stream->lock);
                uint8_t byte;
                ssize_t count = recv(client->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                int error = errno;
                pthread_mutex_lock(&stream->lock);
                if (count >= 0 || (error != EAGAIN && error != EWOULDBLOCK && error != EINTR))
                    atomic_store(&client->stopping, true);
            }
        }
        if (atomic_load(&client->stopping)) break;
        Record *record = client->head;
        client->head = record->next;
        if (client->head == NULL) client->tail = NULL;
        pthread_mutex_unlock(&stream->lock);
        int error = send_record(client, record);
        pthread_mutex_lock(&stream->lock);
        client->bytes -= record->size;
        --client->records;
        free(record);
        if (error != 0) {
            if (error != ECANCELED && !atomic_load(&stream->stopping)) ++stream->stats.send_errors;
            break;
        }
    }
    clear_pending(client);
    int fd = client->fd;
    pthread_mutex_unlock(&stream->lock);
    close(fd);
    pthread_mutex_lock(&stream->lock);
    client->fd = -1;
    client->active = false;
    pthread_mutex_unlock(&stream->lock);
    return NULL;
}

static void reap(L3Stream *stream)
{
    for (size_t i = 0; i < CLIENTS; ++i) {
        Client *client = &stream->clients[i];
        pthread_mutex_lock(&stream->lock);
        bool done = client->joinable && !client->active;
        pthread_t thread = client->thread;
        if (done) client->joinable = false;
        pthread_mutex_unlock(&stream->lock);
        if (done) (void)pthread_join(thread, NULL);
    }
}

static void accept_client(L3Stream *stream, size_t kind)
{
    int fd = accept(stream->listeners[kind], NULL, NULL);
    if (fd < 0) return;
    if (nonblocking(fd) != 0) { close(fd); return; }
    int send_buffer = 16 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    reap(stream);
    pthread_mutex_lock(&stream->lock);
    for (size_t i = kind * 2; i < kind * 2 + 2; ++i) {
        Client *client = &stream->clients[i];
        if (client->joinable) continue;
        client->fd = fd;
        client->active = true;
        client->ready = false;
        atomic_store(&client->stopping, false);
        int error = pthread_create(&client->thread, NULL, writer, client);
        if (error == 0) {
            client->joinable = true;
            ++stream->stats.connections;
            pthread_mutex_unlock(&stream->lock);
            return;
        }
        client->active = false;
        client->fd = -1;
        break;
    }
    pthread_mutex_unlock(&stream->lock);
    close(fd);
}

static void *accept_clients(void *argument)
{
    L3Stream *stream = argument;
    while (!atomic_load(&stream->stopping)) {
        struct pollfd fds[2] = {
            { .fd = stream->listeners[0], .events = POLLIN },
            { .fd = stream->listeners[1], .events = POLLIN }
        };
        int rc = poll(fds, 2, 100);
        if (rc > 0) {
            for (size_t i = 0; i < 2; ++i)
                if (fds[i].revents & POLLIN) accept_client(stream, i);
        }
        reap(stream);
    }
    return NULL;
}

L3Stream *l3_stream_start(const L3StreamConfig *config)
{
    if (config == NULL || config->send_timeout_ms > 60000 ||
        config->point_queue_bytes > 16U * 1024U * 1024U ||
        config->video_queue_bytes > 16U * 1024U * 1024U) { errno = EINVAL; return NULL; }
    L3Stream *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) return NULL;
    stream->listeners[0] = stream->listeners[1] = -1;
    atomic_init(&stream->stopping, false);
    int error = pthread_mutex_init(&stream->lock, NULL);
    if (error != 0) { free(stream); errno = error; return NULL; }
    size_t initialized = 0;
    for (; initialized < CLIENTS; ++initialized) {
        Client *client = &stream->clients[initialized];
        client->owner = stream;
        client->fd = -1;
        client->video = initialized >= 2;
        client->limit = client->video ? config->video_queue_bytes : config->point_queue_bytes;
        if (client->limit == 0) client->limit = client->video ? 2U * 1024U * 1024U : 1024U * 1024U;
        atomic_init(&client->stopping, false);
        error = pthread_cond_init(&client->available, NULL);
        if (error != 0) goto failed;
    }
    stream->timeout_ms = config->send_timeout_ms == 0 ? 1000 : config->send_timeout_ms;
    const char *ip = config->bind_ip == NULL ? "127.0.0.1" : config->bind_ip;
    stream->listeners[0] = listener(ip, config->point_port, &stream->stats.point_port);
    if (stream->listeners[0] < 0) { error = errno; goto failed; }
    stream->listeners[1] = listener(ip, config->video_port, &stream->stats.video_port);
    if (stream->listeners[1] < 0) { error = errno; goto failed; }
    error = pthread_create(&stream->acceptor, NULL, accept_clients, stream);
    if (error == 0) return stream;
failed:
    for (size_t i = 0; i < 2; ++i) if (stream->listeners[i] >= 0) close(stream->listeners[i]);
    for (size_t i = 0; i < initialized; ++i) pthread_cond_destroy(&stream->clients[i].available);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    errno = error;
    return NULL;
}

static uint32_t u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool l3_stream_publish_points(L3Stream *stream, const uint8_t *packet, size_t size)
{
    if (stream == NULL || packet == NULL || size < 44 || size > POINT_LIMIT ||
        u32_le(packet) != UINT32_MAX || u32_le(packet + 16) != size - 28 ||
        (size - 28) % 16 != 0) return false;
    uint8_t *framed = malloc(size + 8);
    if (framed == NULL) return false;
    memcpy(framed, "L3P1", 4);
    uint32_t length = htonl((uint32_t)size);
    memcpy(framed + 4, &length, 4);
    memcpy(framed + 8, packet, size);
    pthread_mutex_lock(&stream->lock);
    bool running = !atomic_load(&stream->stopping);
    if (running) {
        ++stream->stats.point_packets;
        for (size_t i = 0; i < 2; ++i) {
            Client *client = &stream->clients[i];
            if (!client->active || atomic_load(&client->stopping)) continue;
            while (client->head != NULL &&
                   (size + 8 > client->limit - client->bytes || client->records == QUEUE_RECORDS)) {
                Record *old = client->head;
                client->head = old->next;
                if (client->head == NULL) client->tail = NULL;
                client->bytes -= old->size;
                --client->records;
                free(old);
                ++stream->stats.point_dropped;
            }
            if (!queue(client, framed, size + 8)) ++stream->stats.point_dropped;
        }
    }
    pthread_mutex_unlock(&stream->lock);
    free(framed);
    return running;
}

static bool cache_parameter(uint8_t **cache, size_t *cached_size, const uint8_t *nal, size_t size)
{
    if (size > PARAMETER_LIMIT) return false;
    uint8_t *replacement = realloc(*cache, size);
    if (replacement == NULL) return false;
    memcpy(replacement, nal, size);
    *cache = replacement;
    *cached_size = size;
    return true;
}

/* nal includes a normalized four-byte start code. first_mb_in_slice is an
 * unsigned Exp-Golomb value; zero has the one-bit encoding 1, so this check
 * cannot accidentally admit a later slice in a multi-slice IDR picture. */
static bool emit_nal(L3Stream *stream, const uint8_t *nal, size_t size)
{
    if (size <= 4) return true;
    unsigned int type = nal[4] & 31U;
    if (type == 7 && !cache_parameter(&stream->sps, &stream->sps_size, nal, size)) return false;
    if (type == 8 && !cache_parameter(&stream->pps, &stream->pps_size, nal, size)) return false;
    bool first_idr = type == 5 && size > 5 && (nal[5] & 0x80U) != 0;
    for (size_t i = 2; i < CLIENTS; ++i) {
        Client *client = &stream->clients[i];
        if (!client->active || atomic_load(&client->stopping)) continue;
        if (!client->ready) {
            if (!first_idr || stream->sps_size == 0 || stream->pps_size == 0) continue;
            size_t needed = stream->sps_size + stream->pps_size + size;
            if (needed > client->limit - client->bytes || client->records > QUEUE_RECORDS - 3 ||
                !queue(client, stream->sps, stream->sps_size) ||
                !queue(client, stream->pps, stream->pps_size) || !queue(client, nal, size)) {
                disconnect_slow(client);
                continue;
            }
            client->ready = true;
        } else if (!queue(client, nal, size)) disconnect_slow(client);
    }
    return true;
}

bool l3_stream_publish_h264(L3Stream *stream, const uint8_t *data, size_t size)
{
    if (stream == NULL || data == NULL || size == 0) return false;
    pthread_mutex_lock(&stream->lock);
    bool ok = !stream->video_failed && !atomic_load(&stream->stopping);
    if (ok) stream->stats.video_bytes += size;
    for (size_t i = 0; ok && i < size; ++i) {
        uint8_t byte = data[i];
        if (!stream->have_start) {
            if (byte == 1 && stream->zeros >= 2) {
                memcpy(stream->nal, "\0\0\0\1", 4);
                stream->nal_size = 4;
                stream->have_start = true;
            }
        } else {
            if (stream->nal_size == NAL_LIMIT) { ok = false; break; }
            stream->nal[stream->nal_size++] = byte;
            if (byte == 1 && stream->zeros >= 2) {
                size_t start_size = stream->zeros >= 3 ? 4 : 3;
                ok = emit_nal(stream, stream->nal, stream->nal_size - start_size);
                memcpy(stream->nal, "\0\0\0\1", 4);
                stream->nal_size = 4;
            }
        }
        stream->zeros = byte == 0 ? (stream->zeros < 4 ? stream->zeros + 1 : 4) : 0;
    }
    if (!ok && !stream->video_failed) {
        stream->video_failed = true;
        for (size_t i = 2; i < CLIENTS; ++i)
            if (stream->clients[i].active) disconnect_slow(&stream->clients[i]);
    }
    pthread_mutex_unlock(&stream->lock);
    return ok;
}

void l3_stream_stats(L3Stream *stream, L3StreamStats *stats)
{
    if (stream == NULL || stats == NULL) return;
    pthread_mutex_lock(&stream->lock);
    *stats = stream->stats;
    stats->point_clients = stats->video_clients = 0;
    for (size_t i = 0; i < CLIENTS; ++i) {
        if (!stream->clients[i].active || atomic_load(&stream->clients[i].stopping)) continue;
        if (i < 2) ++stats->point_clients;
        else ++stats->video_clients;
    }
    pthread_mutex_unlock(&stream->lock);
}

void l3_stream_stop(L3Stream *stream)
{
    if (stream == NULL) return;
    atomic_store(&stream->stopping, true);
    (void)pthread_join(stream->acceptor, NULL);
    pthread_mutex_lock(&stream->lock);
    for (size_t i = 0; i < CLIENTS; ++i) {
        atomic_store(&stream->clients[i].stopping, true);
        pthread_cond_signal(&stream->clients[i].available);
    }
    pthread_mutex_unlock(&stream->lock);
    for (size_t i = 0; i < CLIENTS; ++i) {
        if (stream->clients[i].joinable) (void)pthread_join(stream->clients[i].thread, NULL);
        pthread_cond_destroy(&stream->clients[i].available);
    }
    close(stream->listeners[0]);
    close(stream->listeners[1]);
    free(stream->sps);
    free(stream->pps);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}
