#pragma once
/* Host stub for esp_err.h */
#include <stddef.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NOT_FOUND 5
#define ESP_ERR_INVALID_SIZE 6
#define ESP_ERR_INVALID_CRC 7

const char *esp_err_to_name(esp_err_t err);
