#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, uint32_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t lock);
