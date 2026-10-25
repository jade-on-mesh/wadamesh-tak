#pragma once
#include "esp_system.h"
using gpio_num_t = int;
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
