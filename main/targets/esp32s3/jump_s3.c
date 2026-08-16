// JMP zone for ESP32-S3: relocated to high IRAM (0x403A0000) by loader_high.ld.
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "soc/systimer_struct.h"
#include "soc/ext_mem_defs.h"

#include "loader_config.h"
#include "jump.h"

#define JMP_ZONE_TEXT __attribute__((section(".jmp_zone.text")))
#define JMP_ZONE_BSS  __attribute__((section(".jmp_zone.bss")))
#define JMP_ZONE_STACK __attribute__((section(".jmp_zone.stack")))

#define JMP_ZONE_BASE 0x403A0000
#define JMP_ZONE_END  0x403B8000

#include "xt_instr_macros.h"

// JMP-zone variables (resident in high-DRAM jmp_bss, clear of payload copy targets)
JMP_ZONE_BSS pending_copy_t safe_copies[MAX_DIRECT_COPIES];
JMP_ZONE_BSS int            safe_copy_count;
JMP_ZONE_BSS uint32_t       safe_entry_addr;
JMP_ZONE_BSS mmu_mapping_t  safe_mappings[MAX_MMU_MAPPINGS];
JMP_ZONE_BSS int            safe_mapping_count;

JMP_ZONE_BSS uint8_t       *fake_flash_ptr;
JMP_ZONE_BSS uint32_t       fake_flash_len;

// Dedicated stack for the JMP zone and payload's early boot.
JMP_ZONE_STACK static uint8_t jump_stack[16384] __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
// ROM-safe helpers (stack strings only; callable with flash remapped away)
// ---------------------------------------------------------------------------
static void JMP_ZONE_TEXT bootloader_uart0_print(const char *str)
{
    if (!str) return;

    volatile uint32_t *uart0_fifo   = (volatile uint32_t *)0x60000000;
    volatile uint32_t *uart0_status = (volatile uint32_t *)0x6000001C;

    while (*str) {
        char ch = *str++;
        uint32_t status = *uart0_status;
        if (status == 0xFFFFFFFF) {
            *uart0_fifo = (uint32_t)ch;
        } else {
            volatile uint32_t timeout = 100000;
            while ((((status = *uart0_status) >> 16) & 0xFF) >= 126 && --timeout > 0) {
                if (status == 0xFFFFFFFF) break;
            }
            *uart0_fifo = (uint32_t)ch;
        }
    }
}

static void JMP_ZONE_TEXT dbg_print_hex(uint32_t val)
{
    char hex[]    = "0x00000000";
    char digits[] = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) {
        hex[i] = digits[val & 0xF];
        val >>= 4;
    }
    bootloader_uart0_print(hex);
}

// ---------------------------------------------------------------------------
// do_mmu_mapping_and_jump — point of no return
// ---------------------------------------------------------------------------
void JMP_ZONE_TEXT __attribute__((noreturn)) do_mmu_mapping_and_jump(void)
{
    // 1. Mask all interrupts (Xtensa level 15)
    asm volatile ("rsil a2, 15" ::: "memory");

    // 2. Switch stack to dedicated jump_stack in high IRAM/DRAM
    uint32_t sp_top = (uint32_t)(jump_stack + sizeof(jump_stack)) - SAVE_AREA_OFFSET;
    SET_STACK(sp_top);

    // 3. Clear CPU watchpoints
    esp_cpu_clear_watchpoint(0);
    esp_cpu_clear_watchpoint(1);

    // 4. Disable watchdogs
    {
        wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
        wdt_hal_write_protect_disable(&rwdt_ctx);
        wdt_hal_disable(&rwdt_ctx);
        wdt_hal_write_protect_enable(&rwdt_ctx);
    }

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    // 5. Silence SYSTIMER and timer-group interrupts
    SYSTIMER.int_ena.val = 0;
    SYSTIMER.int_clr.val = 0xFFFFFFFF;
    TIMERG0.int_ena_timers.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.int_ena_timers.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;

    // 6. Flush PSRAM staging buffer D-cache to physical memory
    if (safe_mapping_count > 0 && fake_flash_ptr != NULL) {
        extern int Cache_WriteBack_Addr(uint32_t vaddr, uint32_t size);
        Cache_WriteBack_Addr((uint32_t)fake_flash_ptr, fake_flash_len);
    }

    // 7. Copy direct segments into internal SRAM (IRAM0 / DRAM / RTC)
    // IRAM0 space and RTC RAM spaces must use 32-bit word stores (s32i).
    for (int i = 0; i < safe_copy_count; i++) {
        uint32_t dest_addr = (uint32_t)safe_copies[i].dest;
        const uint8_t *src = (const uint8_t *)safe_copies[i].src;
        uint32_t len = safe_copies[i].len;

        if ((dest_addr >= 0x40370000 && dest_addr < 0x403E0000) ||
            (dest_addr >= 0x50000000 && dest_addr < 0x50020000) ||
            (dest_addr >= 0x600FE000 && dest_addr < 0x60100000)) {
            volatile uint32_t *d32 = (volatile uint32_t *)dest_addr;
            for (uint32_t j = 0; j < len; j += 4) {
                uint32_t word = 0;
                uint32_t remain = len - j;
                if (remain >= 4) {
                    word = (uint32_t)src[j] |
                           ((uint32_t)src[j+1] << 8) |
                           ((uint32_t)src[j+2] << 16) |
                           ((uint32_t)src[j+3] << 24);
                } else {
                    for (uint32_t k = 0; k < remain; k++) {
                        word |= ((uint32_t)src[j+k] << (k * 8));
                    }
                }
                d32[j / 4] = word;
            }
        } else {
            uint8_t *d = (uint8_t *)dest_addr;
            for (uint32_t j = 0; j < len; j++) d[j] = src[j];
        }
    }

    // 8. Program shared I/D MMU table (DR_REG_MMU_TABLE = 0x600C5000)
    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t first_entry = (safe_mappings[i].vaddr & SOC_MMU_LINEAR_ADDR_MASK) >> 16;
        uint32_t paddr_page  = safe_mappings[i].paddr >> 16;
        uint32_t pages       = (safe_mappings[i].len + 0xFFFF) / 0x10000;

        for (uint32_t e = 0; e < pages; e++) {
            *(volatile uint32_t *)(DR_REG_MMU_TABLE + (first_entry + e) * 4) =
                (paddr_page + e) | SOC_MMU_ACCESS_SPIRAM;
        }
    }

    // 9. Invalidate both instruction and data caches
    extern void Cache_Invalidate_ICache_All(void);
    extern void Cache_Invalidate_DCache_All(void);
    Cache_Invalidate_ICache_All();
    Cache_Invalidate_DCache_All();
    asm volatile ("isync" ::: "memory");

    // 10. Drain UART0 TX FIFO
    volatile uint32_t *uart_status = (volatile uint32_t *)0x6000001C;
    volatile uint32_t drain_timeout = 100000;
    while (((*uart_status >> 16) & 0xFF) > 0 && --drain_timeout > 0) {}

    // 11. Jump to payload entry point
    typedef void (*entry_t)(void) __attribute__((noreturn));
    ((entry_t)safe_entry_addr)();

    while (1);
}
