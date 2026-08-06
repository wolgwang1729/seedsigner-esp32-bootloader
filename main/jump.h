#pragma once

#include <stdint.h>
#include "loader_config.h"

// Deferred copy / MMU mapping records. Built in normal cache context (Steps 4-6
// of app_main), then consumed by the cache-off JMP zone inside
// do_mmu_mapping_and_jump(), which must run without any L1-cached state.
typedef struct { void *dest; void *src; uint32_t len; } pending_copy_t;
typedef struct { uint32_t vaddr; uint32_t paddr; uint32_t len; } mmu_mapping_t;

// RTC RAM survives a software reset (not a power cycle). Defined with
// RTC_DATA_ATTR in main.c; populated by app_main / esp_image_plan() before the
// jump.
extern pending_copy_t safe_copies[MAX_DIRECT_COPIES];
extern int            safe_copy_count;
extern uint32_t       safe_entry_addr;
extern mmu_mapping_t  safe_mappings[MAX_MMU_MAPPINGS];
extern int            safe_mapping_count;

// Naked trampoline: switches SP onto jump_stack and calls
// do_mmu_mapping_and_jump(). Point of no return.
void do_mmu_mapping_and_jump_trampoline(void) __attribute__((noreturn));
