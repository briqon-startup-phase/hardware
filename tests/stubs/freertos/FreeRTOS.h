#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) (ms)
void bq_test_enter(void);
void bq_test_exit(void);
#define portENTER_CRITICAL(mux) ((void)(mux), bq_test_enter())
#define portEXIT_CRITICAL(mux) ((void)(mux), bq_test_exit())
