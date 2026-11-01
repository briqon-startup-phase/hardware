#pragma once
#include <stdint.h>
#include "esp_err.h"
enum { GPIO_MODE_INPUT=1, GPIO_MODE_INPUT_OUTPUT_OD=2, GPIO_PULLUP_ENABLE=1 };
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_set_level(int pin, uint32_t level);
int gpio_get_level(int pin);
