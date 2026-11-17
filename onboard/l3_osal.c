#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "l3_osal.h"
#include "dji_error.h"
#include "osal/osal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_t thread;
    void *(*function)(void *);
    void *argument;
    char name[16];
} Task;

static void *run_task(void *raw)
{
    Task *task = raw;
    /* Cancellation belongs to the NEW thread. Keep deferred cancellation so
     * an unrelated allocator/mutex operation cannot be interrupted mid-update. */
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    if (task->name[0]) (void)pthread_setname_np(pthread_self(), task->name);
    return task->function(task->argument);
}

T_DjiReturnCode L3Osal_TaskCreate(const char *name, void *(*function)(void *),
                                uint32_t stack_size, void *argument, T_DjiTaskHandle *out)
{
    (void)stack_size; /* Like the official Linux adapter, use pthread defaults. */
    if (out == NULL || function == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    T_DjiTaskHandle handle = NULL;
    memcpy(out, &handle, sizeof(handle));
    Task *task = calloc(1, sizeof(*task));
    if (task == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_MEMORY_ALLOC_FAILED;
    task->function = function;
    task->argument = argument;
    if (name != NULL) strncpy(task->name, name, sizeof(task->name) - 1);
    if (pthread_create(&task->thread, NULL, run_task, task) != 0) {
        free(task);
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    }
    handle = task;
    /* PSDK passes addresses of packed handle fields on ARM64. */
    memcpy(out, &handle, sizeof(handle));
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode L3Osal_TaskDestroy(T_DjiTaskHandle raw)
{
    Task *task = raw;
    if (task == NULL || pthread_equal(task->thread, pthread_self()))
        return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    if (pthread_cancel(task->thread) != 0 || pthread_join(task->thread, NULL) != 0)
        return DJI_ERROR_SYSTEM_MODULE_CODE_SYSTEM_ERROR;
    /* Deinit may now release task-owned state: the worker has really exited. */
    free(task);
    return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
}

T_DjiReturnCode L3Osal_MutexCreate(T_DjiMutexHandle *out)
{
    if (out == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    T_DjiMutexHandle handle = NULL;
    T_DjiReturnCode code = Osal_MutexCreate(&handle);
    memcpy(out, &handle, sizeof(handle));
    return code;
}

T_DjiReturnCode L3Osal_SemaphoreCreate(uint32_t initial, T_DjiSemaHandle *out)
{
    if (out == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_INVALID_PARAMETER;
    T_DjiSemaHandle handle = NULL;
    T_DjiReturnCode code = Osal_SemaphoreCreate(initial, &handle);
    memcpy(out, &handle, sizeof(handle));
    return code;
}

T_DjiReturnCode L3Osal_SemaphoreDestroy(T_DjiSemaHandle semaphore)
{
    /* CameraManager unconditionally destroys its unused media-downloader's
     * NULL semaphore. There is no resource to release: make destruction
     * idempotent, like free(NULL), rather than invoking sem_destroy(NULL). */
    if (semaphore == NULL) return DJI_ERROR_SYSTEM_MODULE_CODE_SUCCESS;
    return Osal_SemaphoreDestroy(semaphore);
}
