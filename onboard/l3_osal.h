#ifndef L3_OSAL_H
#define L3_OSAL_H
#include "dji_platform.h"

T_DjiReturnCode L3Osal_TaskCreate(const char *name, void *(*function)(void *),
                                uint32_t stack_size, void *argument, T_DjiTaskHandle *out);
T_DjiReturnCode L3Osal_TaskDestroy(T_DjiTaskHandle task);
T_DjiReturnCode L3Osal_MutexCreate(T_DjiMutexHandle *out);
T_DjiReturnCode L3Osal_SemaphoreCreate(uint32_t initial, T_DjiSemaHandle *out);
T_DjiReturnCode L3Osal_SemaphoreDestroy(T_DjiSemaHandle semaphore);
#endif
