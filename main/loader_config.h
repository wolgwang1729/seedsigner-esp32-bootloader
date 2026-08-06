#pragma once

// Shared loader-wide limits. Kept in one place so the SD-card loader, image
// parser, and RTC-RAM hand-off all agree on the same bounds.
#define MAX_FIRMWARE_SIZE (8 * 1024 * 1024)
