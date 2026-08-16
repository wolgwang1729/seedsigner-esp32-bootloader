// JMP zone for ESP32-P4: RISC-V naked trampoline and cache eviction.
// Relocated above payload region by loader_high.ld.
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_rom_uart.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_periph.h"
#include "soc/systimer_struct.h"
#include "soc/spi_mem_c_reg.h"
#include "soc/spi_mem_s_reg.h"
#include "riscv/rv_utils.h"

#include "loader_config.h"
#include "jump.h"

// RTC RAM survives a software reset (not a power cycle).
RTC_DATA_ATTR pending_copy_t safe_copies[MAX_DIRECT_COPIES];
RTC_DATA_ATTR int            safe_copy_count    = 0;
RTC_DATA_ATTR uint32_t       safe_entry_addr    = 0;
RTC_DATA_ATTR mmu_mapping_t  safe_mappings[MAX_MMU_MAPPINGS];
RTC_DATA_ATTR int            safe_mapping_count = 0;

// Scratch buffer used to thrash the L1 D-cache before the jump. Relocated above
// the payload region (0x4FF40000+) by loader_high.ld.
static uint32_t evict_buf[262144 / 4];

// Dedicated stack for the JMP zone and the payload's early boot.
static uint8_t jump_stack[32768] __attribute__((aligned(16)));

// ---------------------------------------------------------------------------
// ROM-safe helpers (stack strings only; callable with cache disabled)
// ---------------------------------------------------------------------------
static void RTC_IRAM_ATTR bootloader_uart0_print(const char *str)
{
    if (!str) return;

    volatile uint32_t *uart0_fifo   = (volatile uint32_t *)0x500CA000;
    volatile uint32_t *uart0_status = (volatile uint32_t *)0x500CA01C;

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

static void RTC_IRAM_ATTR dbg_print_hex(uint32_t val)
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
void RTC_IRAM_ATTR __attribute__((noreturn)) do_mmu_mapping_and_jump(void)
{
    char msg5[]   = "JMP[7] JUMP!\r\n";
    char m_enter[] = "JMP[1] entered\r\n";
    bootloader_uart0_print(m_enter);

    portDISABLE_INTERRUPTS();

    // Clear FreeRTOS hardware watchpoints / PMP
    esp_cpu_clear_watchpoint(0);
    esp_cpu_clear_watchpoint(1);

    // Disable all watchdogs (SWD, LP_WDT, MWDT0, MWDT1, RWDT)
    REG_WRITE(0x50116020, 0x50D83AA1);
    REG_WRITE(0x5011601C, (1U << 31) | (1U << 30) | (1U << 19) | (1U << 18));
    REG_WRITE(0x50116020, 0);

    REG_WRITE(0x50116018, 0x50D83AA1);
    REG_WRITE(0x50116014, (1U << 31));
    REG_CLR_BIT(0x50116000, (1U << 31) | (0xFFFU << 19) | (1U << 12));
    REG_WRITE(0x5011602C, 0);
    REG_WRITE(0x50116030, 0xFFFFFFFF);
    REG_WRITE(0x50116018, 0);

    TIMERG0.wdtwprotect.val    = 0x50D83AA1;
    TIMERG0.wdtfeed.val        = 1;
    TIMERG0.wdtconfig0.val     = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG0.wdtwprotect.val    = 0;

    TIMERG1.wdtwprotect.val    = 0x50D83AA1;
    TIMERG1.wdtfeed.val        = 1;
    TIMERG1.wdtconfig0.val     = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.wdtwprotect.val    = 0;

    wdt_hal_context_t mwdt0_ctx = {.inst = WDT_MWDT0, .mwdt_dev = &TIMERG0};
    wdt_hal_write_protect_disable(&mwdt0_ctx);
    wdt_hal_disable(&mwdt0_ctx);
    wdt_hal_write_protect_enable(&mwdt0_ctx);

    wdt_hal_context_t mwdt1_ctx = {.inst = WDT_MWDT1, .mwdt_dev = &TIMERG1};
    wdt_hal_write_protect_disable(&mwdt1_ctx);
    wdt_hal_disable(&mwdt1_ctx);
    wdt_hal_write_protect_enable(&mwdt1_ctx);

    wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt_ctx);
    wdt_hal_disable(&rwdt_ctx);
    wdt_hal_write_protect_enable(&rwdt_ctx);

    // Silence SYSTIMER and timer-group interrupts
    SYSTIMER.int_ena.val  = 0;
    SYSTIMER.int_clr.val  = 0x7;
    SYSTIMER.conf.val    &= ~((1U << 22) | (1U << 23) | (1U << 24));
    TIMERG0.int_ena_timers.val = 0;
    TIMERG0.int_clr_timers.val = 0xFFFFFFFF;
    TIMERG1.int_ena_timers.val = 0;
    TIMERG1.int_clr_timers.val = 0xFFFFFFFF;

    char m_wdt[] = "JMP[2] WDT and SysTick disabled\r\n";
    bootloader_uart0_print(m_wdt);

    asm volatile ("csrw mie, zero\n");
    rv_utils_intr_global_disable();

    char m_intr[] = "JMP[3] interrupts off, starting cache eviction\r\n";
    bootloader_uart0_print(m_intr);

    // Thrash full 64 KB L1 D-cache to force eviction before MMU remap
    volatile uint32_t *evict_ptr = evict_buf;
    for (int i = 0; i < (65536 / 4); i++) evict_ptr[i] = i;
    char m_wb[] = "JMP[4] D-cache evicted\r\n";
    bootloader_uart0_print(m_wb);

    // Copy direct segments into internal SRAM (source is in PSRAM — cache still up)
    for (int i = 0; i < safe_copy_count; i++) {
        uint8_t *d = (uint8_t *)safe_copies[i].dest;
        uint8_t *s = (uint8_t *)safe_copies[i].src;
        for (uint32_t j = 0; j < safe_copies[i].len; j++) d[j] = s[j];
    }

    char m_copy[] = "JMP[5] copies done, entry bytes: ";
    bootloader_uart0_print(m_copy);
    dbg_print_hex(*(volatile uint32_t *)safe_entry_addr);
    char m_nl[] = "\r\n";
    bootloader_uart0_print(m_nl);

    // Program MMU entries for PSRAM-mapped segments
    for (int i = 0; i < safe_mapping_count; i++) {
        uint32_t page_size = 65536;
        uint32_t page_num  = (safe_mappings[i].len + page_size - 1) / page_size;
        uint32_t vaddr     = safe_mappings[i].vaddr;
        uint32_t mmu_val   = safe_mappings[i].paddr >> 16;

        char m_map[] = "JMP[6] MMU map: vaddr=";
        bootloader_uart0_print(m_map);
        dbg_print_hex(vaddr);
        char m_pa[] = " paddr=";
        bootloader_uart0_print(m_pa);
        dbg_print_hex(safe_mappings[i].paddr);
        char m_pg[] = " pages=";
        bootloader_uart0_print(m_pg);
        dbg_print_hex(page_num);
        bootloader_uart0_print(m_nl);

        while (page_num--) {
            uint32_t entry_id    = (vaddr & 0x03FFFFFF) >> 16;
            uint32_t index_reg   = SPI_MEM_C_MMU_ITEM_INDEX_REG;
            uint32_t content_reg = SPI_MEM_C_MMU_ITEM_CONTENT_REG;
            uint32_t final_val   = mmu_val | (1 << 10);

            if (vaddr >= 0x48000000) {
                index_reg   = SPI_MEM_S_MMU_ITEM_INDEX_REG;
                content_reg = SPI_MEM_S_MMU_ITEM_CONTENT_REG;
                // PSRAM: valid | access | no sensitive (plaintext)
                final_val   = mmu_val | (1 << 11) | (1 << 10);
            }

            char m_ent[] = "  entry=";
            bootloader_uart0_print(m_ent);
            dbg_print_hex(entry_id);
            char m_val[] = " val=";
            bootloader_uart0_print(m_val);
            dbg_print_hex(final_val);
            REG_WRITE(index_reg, entry_id);
            REG_WRITE(content_reg, final_val);
            char m_ok[] = " OK\r\n";
            bootloader_uart0_print(m_ok);

            vaddr += page_size;
            mmu_val++;
        }
    }

    // Invalidate caches through ROM API
    extern void Cache_Invalidate_Addr(uint32_t map, uint32_t vaddr, uint32_t size);
    for (int i = 0; i < safe_mapping_count; i++) {
        Cache_Invalidate_Addr(0x10, safe_mappings[i].vaddr, safe_mappings[i].len);
        Cache_Invalidate_Addr(0x20, safe_mappings[i].vaddr, safe_mappings[i].len);
    }
    asm volatile ("fence.i\n" "fence rw,rw\n");

    // Second D-cache drain pass
    for (int i = 0; i < (65536 / 4); i++) evict_ptr[i] = i;

    // Zero out mtvec so early traps in the payload don't vector to loader ISRs
    asm volatile ("csrw mtvec, zero\n");

    bootloader_uart0_print(msg5);

    // Drain UART0 FIFO before jumping
    volatile uint32_t *uart0_status = (volatile uint32_t *)0x500CA01C;
    volatile uint32_t drain_timeout = 100000;
    while (((*uart0_status >> 16) & 0xFF) > 0 && --drain_timeout > 0) {}

    typedef void (*entry_t)(void) __attribute__((noreturn));
    entry_t target_entry = (entry_t)safe_entry_addr;
    target_entry();

    while (1);
}

void __attribute__((naked)) do_mmu_mapping_and_jump_trampoline(void)
{
    asm volatile (
        "csrw mie, zero\n"
        "li   t0, 0x3ff06000\n"
        "li   t2, -1\n"
        "sw   t2, 0x3c(t0)\n"    /* SP_MAX_REG  = 0xFFFFFFFF */
        "sw   zero, 0x38(t0)\n"  /* SP_MIN_REG  = 0          */
        "sw   zero, 0x0c(t0)\n"  /* INTR_CLR_REG: clear any latched spill */
        "lw   t3, 0(t0)\n"
        "li   t1, 0x300\n"
        "not  t1, t1\n"
        "and  t3, t3, t1\n"
        "sw   t3, 0(t0)\n"       /* clear SP_SPILL_MIN/MAX_ENA */
        "mv   sp, %0\n"
        "call do_mmu_mapping_and_jump\n"
        :
        : "r"(jump_stack + sizeof(jump_stack))
    );
}
