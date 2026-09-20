#pragma once
#include <stdint.h>
typedef int BaseType_t;
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
