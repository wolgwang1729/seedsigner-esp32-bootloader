#pragma once

// Shared loader-wide limits. Kept in one place so the SD-card loader, image
// parser, and RTC/high-RAM hand-off all agree on the same bounds.
#define MAX_FIRMWARE_SIZE  (8 * 1024 * 1024)
#define MAX_SEGMENT_COUNT  16
#define MAX_MMU_MAPPINGS   20
#define MAX_DIRECT_COPIES  20
#define BIP39_WORD_COUNT   4
