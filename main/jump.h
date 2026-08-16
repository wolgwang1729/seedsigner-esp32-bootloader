#pragma once

#include <stdint.h>
#include "loader_config.h"

// Deferred copy / MMU mapping records. Built in normal cache context (Steps 4-6
// of app_main), then consumed by the cache-off JMP zone inside
// do_mmu_mapping_and_jump().
typedef struct { void *dest; void *src; uint32_t len; } pending_copy_t;
typedef struct { uint32_t vaddr; uint32_t paddr; uint32_t len; } mmu_mapping_t;

extern pending_copy_t safe_copies[MAX_DIRECT_COPIES];
extern int            safe_copy_count;
extern uint32_t       safe_entry_addr;
extern mmu_mapping_t  safe_mappings[MAX_MMU_MAPPINGS];
extern int            safe_mapping_count;

#if CONFIG_IDF_TARGET_ESP32S3
extern uint8_t       *fake_flash_ptr;
extern uint32_t       fake_flash_len;
#endif

// Point of no return: disables interrupts/WDTs, copies direct segments,
// reconfigures MMU, flushes/invalidates caches, and jumps to entry point.
void do_mmu_mapping_and_jump(void) __attribute__((noreturn));

#if CONFIG_IDF_TARGET_ESP32P4
// RISC-V naked trampoline that switches SP onto jump_stack before calling do_mmu_mapping_and_jump
void do_mmu_mapping_and_jump_trampoline(void) __attribute__((noreturn));
#endif
