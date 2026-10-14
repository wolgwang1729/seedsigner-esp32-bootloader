#pragma once
/* Host stub for esp_mmu_map.h */
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef int mmu_target_t;
esp_err_t esp_mmu_vaddr_to_paddr(const void *vaddr, uint32_t *paddr,
                                 mmu_target_t *target);
