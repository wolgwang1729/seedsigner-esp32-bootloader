#pragma once
/* Host stub for esp_random.h */
#include <stddef.h>
#include <stdint.h>
void esp_fill_random(void *buf, size_t len);
uint32_t esp_random(void);
