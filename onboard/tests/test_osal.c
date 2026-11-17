#define _POSIX_C_SOURCE 200809L
#include "l3_osal.h"
#include "dji_error.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Stub only the existing mutex/semaphore allocator; task lifecycle is real. */
T_DjiReturnCode Osal_MutexCreate(T_DjiMutexHandle *out)
{
    assert((uintptr_t)out % _Alignof(void *) == 0);
    *out = malloc(1);
    assert(*out != NULL);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}
T_DjiReturnCode Osal_SemaphoreCreate(uint32_t initial, T_DjiSemaHandle *out)
{
    assert(initial == 3);
    return Osal_MutexCreate(out);
}
T_DjiReturnCode Osal_SemaphoreDestroy(T_DjiSemaHandle semaphore)
{
    assert(semaphore != NULL);
    free(semaphore);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

static atomic_bool ready, finished;
static void cleanup(void *unused)
{
    (void)unused;
    struct timespec pause = {.tv_nsec = 20000000};
    nanosleep(&pause, NULL);
    atomic_store(&finished, true);
}
static void *worker(void *unused)
{
    (void)unused;
    pthread_cleanup_push(cleanup, NULL);
    atomic_store(&ready, true);
    for (;;) {
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    pthread_cleanup_pop(1);
    return NULL;
}

int main(void)
{
    _Alignas(void *) unsigned char packed[sizeof(void *) + 2];
    memset(packed, 0xa5, sizeof(packed));
    void *out = packed + 1;
    assert(L3Osal_MutexCreate(out) == 0);
    void *handle;
    memcpy(&handle, out, sizeof(handle));
    free(handle);
    assert(L3Osal_SemaphoreCreate(3, out) == 0);
    memcpy(&handle, out, sizeof(handle));
    assert(L3Osal_SemaphoreDestroy(handle) == 0);
    assert(L3Osal_SemaphoreDestroy(NULL) == 0);
    int previous;
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous);
    assert(L3Osal_TaskCreate("l3-test", worker, 0, NULL, out) == 0);
    int caller_type;
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &caller_type);
    assert(caller_type == PTHREAD_CANCEL_DEFERRED);
    while (!atomic_load(&ready)) {
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    memcpy(&handle, out, sizeof(handle));
    assert(L3Osal_TaskDestroy(handle) == 0);
    assert(atomic_load(&finished)); /* Destroy waits for cleanup, not just cancel. */
    assert(packed[0] == 0xa5 && packed[sizeof(packed) - 1] == 0xa5);
    pthread_setcanceltype(previous, NULL);
    return 0;
}
