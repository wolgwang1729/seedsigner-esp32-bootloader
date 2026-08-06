# SPDX-FileCopyrightText: 2024-2026
# SPDX-License-Identifier: CC0-1.0
#
# Pytest integration test for the SeedSigner Stateless Bootloader (ESP32-P4).
#
# Matches the boot chain in the loader modules (main/storage.c, main/main.c,
# main/esp_image.c, main/jump.c — see docs/code_structure.md):
#   1. SD-card load: mounts FAT32 /sdcard, reads seedsigner_esp32p4.bin into
#      PSRAM, unmounts immediately (TOCTOU-safe)
#   2. Specter secure app loader: bl_section header + platform attr + version
#      check, hash over the main section, secp256k1 multisig verification
#   3. Anti-phishing proof: SHA-256 over the ~6.8 MB random_fill
#      region, 4 BIP-39 words printed (first boot also provisions the TRNG
#      fill — this test tolerates the ~2 minute provisioning delay)
#   4. Raw ESP32 image parsing + PSRAM MMU footprint + fake_flash staging
#   5. Segment classification and routing (fake_flash vs direct copy)
#   6. Bare-metal jump sequence JMP[1..8] (WDT/interrupt teardown, D-cache
#      eviction, copies, MMU mapping arithmetic, D-cache drain)
#   7. Payload boot through the stateless_shim (rug-pull interceptors) —
#      stock hello-world runs from PSRAM
#   8. Reboot stability: after a hard reset the SAME 4 anti-phishing words
#      must be reported (provisioning is idempotent, digest is stable)
#
# Usage:
#   source ~/esp/esp-idf-v5.5/export.sh
#   pytest --embedded-services esp,idf --target esp32p4 \
#          --port /dev/ttyACM0 pytest_seedsigner_bootloader_p4_stateless_os.py
#
# Prerequisites (physical hardware):
#   - ESP32-P4 board attached on the port above
#   - FAT32 SD card with a Specter-signed bundle at seedsigner_esp32p4.bin
#     (build it with tools/generate_signed_payload.py and copy it to the card)
#
# Architecture reference (ESP32-P4 memory map):
#   Internal SRAM (HP L2MEM):  0x4FF00000 - 0x4FFBFFFF  (768 KB)
#   Flash cache (IROM/DROM):   0x40000000 - 0x44000000  (shared I/D)
#   PSRAM cache:               0x48000000 - 0x4C000000  (via MMU)
#   MMU page size:             64 KB (0x10000)
# ---------------------------------------------------------------------------

import logging
import os
import re
import time

import pytest
from pytest_embedded_idf.app import IdfApp
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize

# ── ESP32-P4 memory map constants ──────────────────────────────────────────
PSRAM_VADDR_START   = 0x48000000
PSRAM_VADDR_END     = 0x4C000000
SRAM_VADDR_START    = 0x4FF00000
SRAM_VADDR_END      = 0x4FFBFFFF
MMU_PAGE_SIZE       = 0x10000       # 64 KB

# MMU entry bit-flags (from ESP32-P4 TRM, Chapter "MMU")
MMU_VALID_BIT       = (1 << 10)     # Access permission / valid
MMU_PSRAM_TYPE_BIT  = (1 << 11)     # Target type: 1=PSRAM, 0=Flash
MMU_PAGE_NUM_MASK   = 0x3FF         # Low 10 bits = physical page number

# Image format constants
MAX_FIRMWARE_SIZE   = 8 * 1024 * 1024
MAX_SEGMENT_COUNT   = 16

# Timeout for the anti-phishing proof line. On the FIRST boot after a flash
# the loader provisions the ~6.8 MB random_fill partition (TRNG fill + hash),
# which takes ~2 minutes; the proof line only prints after that. On later
# boots it prints within seconds. 300 s covers the first-boot path.
ANTI_PHISH_TIMEOUT  = 300

# Minimum set of "rug pull" interceptors that must fire for stateless boot to
# work. These are the ESP-IDF early-boot functions the shim --wraps so that a
# normal app boot cannot re-initialize hardware the loader already set up.
MIN_INTERCEPTOR_COUNT = 5

# Payload reboot cycles (stock hello-world auto-restarts after ~10 s); each
# cycle re-runs the full loader chain, proving the hand-off is stable.
STABILITY_CYCLES = 2


def _decode(val):
    """Decode bytes to str if needed (pexpect match groups may be bytes)."""
    return val.decode() if isinstance(val, bytes) else val


def _load_bip39_wordlist():
    """Parse the 2048 words out of main/bip39_wordlist.c for cross-validation."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "main", "bip39_wordlist.c")
    words = set()
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith('"') and line.endswith('",'):
                words.add(line[1:-2])
    assert len(words) == 2048, f'BIP-39 wordlist has {len(words)} words, expected 2048'
    return words


BIP39_WORDS = _load_bip39_wordlist()


# ── Single monolithic test ─────────────────────────────────────────────────
# pytest-embedded shares the serial stream across expect() calls within one
# test.  Splitting into multiple test functions would require each to re-flash
# and re-boot.  Instead we use a single test with clearly delineated phases
# and sub-assertions so that a failure pinpoints exactly which stage broke.

@pytest.mark.generic
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_stateless_bootloader_and_payload(app: IdfApp, dut: IdfDut) -> None:
    """
    End-to-end integration test for the SeedSigner stateless bootloader.

    Validates SD-card load + Specter multisig verification + the
    anti-phishing proof + JMP[1..8] hand-off + stock hello-world execution
    from PSRAM through the stateless shim.
    """

    # Accumulators for cross-phase validation and final summary
    segments = []           # list of (addr, len, route) tuples from Phase 4
    mmu_maps = []           # list of (vaddr, paddr, pages) from Phase 5
    mmu_entries = []        # list of (entry_id, entry_val) from Phase 5
    interceptors_seen = set()  # interceptor names from Phase 6
    ap_words = None         # 4-tuple of anti-phishing words from Phase 3

    # ── Phase 0: Hard-reset to capture the full boot log ───────────────
    logging.info('Phase 0: Triggering hard reset via DTR/RTS...')
    dut.serial.hard_reset()
    time.sleep(0.5)

    # ====================================================================
    # PHASE 1: SD-CARD LOAD
    # The loader mounts the FAT32 card, reads the whole Specter bundle into
    # PSRAM, and unmounts BEFORE verification (TOCTOU-safe).
    # ====================================================================
    logging.info('Phase 1: SD-card load...')

    dut.expect('SeedSigner Loader — ESP32-P4 PSRAM payload', timeout=15)
    logging.info('  ✓ Loader banner displayed')

    dut.expect('SD card mounted at /sdcard', timeout=10)
    logging.info('  ✓ SD card mounted')

    dut.expect(r'Unmounting SD card before verification \(TOCTOU-safe\)\.\.\.', timeout=10)
    logging.info('  ✓ Unmount before verification (TOCTOU-safe)')

    load_match = dut.expect(
        r'\[SD CARD\] Loaded (\d+) bytes from /sdcard/seedsigner_esp32p4\.bin',
        timeout=10
    )
    loaded_bytes = int(load_match.group(1))
    assert 0 < loaded_bytes <= MAX_FIRMWARE_SIZE, \
        f'Loaded {loaded_bytes} bytes out of valid range (0, {MAX_FIRMWARE_SIZE}]'
    logging.info(f'  ✓ Loaded {loaded_bytes:,} bytes from SD card')

    # ====================================================================
    # PHASE 2: SPECTER SECURE APP LOADER VERIFICATION
    # ====================================================================
    logging.info('Phase 2: Specter multisig verification...')

    dut.expect('Specter bootloader section detected', timeout=10)
    logging.info('  ✓ Specter bundle header detected')

    ver_match = dut.expect(r'Firmware version:\s+(\d+)', timeout=5)
    pl_ver = int(ver_match.group(1))
    assert pl_ver >= 1, f'Firmware version {pl_ver} fails the downgrade check (must be >= 1)'
    logging.info(f'  ✓ Firmware version: {pl_ver}')

    dut.expect('Performing secp256k1 multisig verification...', timeout=5)
    dut.expect('Signature verification PASSED!', timeout=10)
    logging.info('  ✓ secp256k1 multisig signature verified')

    # ====================================================================
    # PHASE 3: PHASE 13 ANTI-PHISHING PROOF
    # Every boot the loader re-hashes the random_fill region and derives
    # 4 BIP-39 words. The first boot also provisions the TRNG fill first
    # (~2 min), which the long timeout below tolerates.
    # ====================================================================
    logging.info('Phase 3: Anti-phishing proof...')

    proof_match = dut.expect(
        r'ANTI-PHISHING PROOF:\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)',
        timeout=ANTI_PHISH_TIMEOUT
    )
    ap_words = tuple(_decode(g) for g in proof_match.groups())
    for w in ap_words:
        assert w in BIP39_WORDS, \
            f'Anti-phishing word "{w}" is not in the BIP-39 wordlist!'
    logging.info(
        f'  ✓ ANTI-PHISHING PROOF: {" ".join(ap_words)} '
        f'(all 4 are valid BIP-39 words)'
    )

    # ====================================================================
    # PHASE 4: IMAGE HEADER + MMU FOOTPRINT + fake_flash
    # ====================================================================
    logging.info('Phase 4: Image parsing & PSRAM footprint...')

    img_match = dut.expect(
        r'Image OK:\s+(\d+)\s+segments,\s+entry=0x([0-9A-Fa-f]+)',
        timeout=10
    )
    segment_count = int(img_match.group(1))
    entry_addr    = int(img_match.group(2), 16)

    assert 1 <= segment_count <= MAX_SEGMENT_COUNT, \
        f'Segment count {segment_count} outside valid range [1, {MAX_SEGMENT_COUNT}]'
    assert PSRAM_VADDR_START <= entry_addr < PSRAM_VADDR_END, \
        f'Entry address 0x{entry_addr:08X} is not in PSRAM range ' \
        f'[0x{PSRAM_VADDR_START:08X}, 0x{PSRAM_VADDR_END:08X})'
    logging.info(f'  ✓ Image header: {segment_count} segments, entry=0x{entry_addr:08X}')

    footprint_match = dut.expect(
        r'PSRAM MMU footprint:\s+(\d+)\s+bytes',
        timeout=5
    )
    mmu_footprint = int(footprint_match.group(1))
    assert mmu_footprint % MMU_PAGE_SIZE == 0, \
        f'MMU footprint {mmu_footprint} is not 64KB-aligned'
    logging.info(f'  ✓ MMU footprint: {mmu_footprint:,} bytes '
                 f'({mmu_footprint // MMU_PAGE_SIZE} pages)')

    ff_match = dut.expect(
        r'fake_flash:\s+vaddr=0x([0-9A-Fa-f]+)\s+paddr=0x([0-9A-Fa-f]+)',
        timeout=5
    )
    ff_paddr = int(ff_match.group(2), 16)
    assert ff_paddr % MMU_PAGE_SIZE == 0, \
        f'fake_flash paddr 0x{ff_paddr:08X} is not 64KB-aligned'
    logging.info(f'  ✓ fake_flash staging: paddr=0x{ff_paddr:08X}')

    # ====================================================================
    # PHASE 5: SEGMENT CLASSIFICATION & ROUTING
    # ====================================================================
    logging.info('Phase 5: Segment classification & routing...')

    psram_segment_count  = 0
    direct_segment_count = 0

    for i in range(segment_count):
        seg_match = dut.expect(
            r'Seg\s+(\d+):\s+addr=0x([0-9A-Fa-f]+)\s+len=(\d+)',
            timeout=5
        )
        seg_idx  = int(seg_match.group(1))
        seg_addr = int(seg_match.group(2), 16)
        seg_len  = int(seg_match.group(3))

        assert seg_idx == i, f'Segment index mismatch: expected {i}, got {seg_idx}'
        assert seg_len > 0, f'Segment {i} has zero length'
        assert seg_len <= MAX_FIRMWARE_SIZE, \
            f'Segment {i} length {seg_len} exceeds MAX_FIRMWARE_SIZE'

        if PSRAM_VADDR_START <= seg_addr < PSRAM_VADDR_END:
            route_match = dut.expect(r'->\s+(fake_flash|direct copy)', timeout=5)
            route = _decode(route_match.group(1))
            assert route == 'fake_flash', \
                f'Segment {i} at 0x{seg_addr:08X} (PSRAM range) routed as {route}!'
            psram_segment_count += 1
        else:
            route_match = dut.expect(r'->\s+(fake_flash|direct copy)', timeout=5)
            route = _decode(route_match.group(1))
            assert route == 'direct copy', \
                f'Segment {i} at 0x{seg_addr:08X} (SRAM range) routed as {route}!'
            direct_segment_count += 1

        segments.append((seg_addr, seg_len, route))
        logging.info(
            f'  ✓ Seg {i}: addr=0x{seg_addr:08X} len={seg_len:,} -> {route}'
        )

    assert psram_segment_count >= 1, \
        'No PSRAM-mapped segments found — payload has no PSRAM XIP code'
    logging.info(
        f'  ✓ Routing: {psram_segment_count} PSRAM (fake_flash), '
        f'{direct_segment_count} direct (SRAM)'
    )

    # ====================================================================
    # PHASE 6: BARE-METAL JUMP SEQUENCE JMP[1..8]
    # ====================================================================
    logging.info('Phase 6: Bare-metal jump sequence...')

    dut.expect(r'JMP\[1\] entered', timeout=5)
    logging.info('  ✓ JMP[1]: Entered bare-metal jump function (RTC_IRAM_ATTR)')

    dut.expect(r'JMP\[2\] WDT and SysTick disabled', timeout=5)
    logging.info('  ✓ JMP[2]: All watchdogs disabled (SWD, LP_WDT, MWDT0/1, RWDT, SysTick)')

    dut.expect(r'JMP\[3\] interrupts off, starting cache eviction', timeout=5)
    logging.info('  ✓ JMP[3]: RISC-V mie=0, global interrupt disabled')

    dut.expect(r'JMP\[4\] D-cache evicted', timeout=5)
    logging.info('  ✓ JMP[4]: L1 D-cache thrashed via 64KB eviction buffer')

    entry_match = dut.expect(
        r'JMP\[5\] copies done, entry bytes: 0x([0-9A-Fa-f]+)',
        timeout=5
    )
    entry_bytes = int(entry_match.group(1), 16)
    assert entry_bytes != 0x00000000, \
        'Entry point reads as 0x00000000 — deferred copy failed to stage code!'
    assert entry_bytes != 0xFFFFFFFF, \
        'Entry point reads as 0xFFFFFFFF — memory not initialized (erased flash pattern)!'
    logging.info(f'  ✓ JMP[5]: Copies completed, entry bytes=0x{entry_bytes:08X} (staged)')

    for m in range(psram_segment_count):
        map_match = dut.expect(
            r'JMP\[6\] MMU map:\s+vaddr=0x([0-9A-Fa-f]+)\s+'
            r'paddr=0x([0-9A-Fa-f]+)\s+pages=0x([0-9A-Fa-f]+)',
            timeout=5
        )
        vaddr      = int(map_match.group(1), 16)
        paddr      = int(map_match.group(2), 16)
        page_count = int(map_match.group(3), 16)

        assert vaddr % MMU_PAGE_SIZE == 0, \
            f'MMU mapping {m}: vaddr 0x{vaddr:08X} is not 64KB-aligned'
        assert PSRAM_VADDR_START <= vaddr < PSRAM_VADDR_END, \
            f'MMU mapping {m}: vaddr 0x{vaddr:08X} outside PSRAM range'
        assert paddr % MMU_PAGE_SIZE == 0, \
            f'MMU mapping {m}: paddr 0x{paddr:08X} is not 64KB-aligned'
        assert page_count >= 1, f'MMU mapping {m}: page count is 0'
        assert paddr >= ff_paddr, \
            f'MMU mapping {m}: paddr 0x{paddr:08X} is below fake_flash base 0x{ff_paddr:08X}'

        mmu_maps.append((vaddr, paddr, page_count))

        for p in range(page_count):
            entry_match = dut.expect(
                r'entry=0x([0-9A-Fa-f]+)\s+val=0x([0-9A-Fa-f]+)\s+OK',
                timeout=5
            )
            entry_id  = int(entry_match.group(1), 16)
            entry_val = int(entry_match.group(2), 16)

            expected_entry = ((vaddr + p * MMU_PAGE_SIZE) & 0x03FFFFFF) >> 16
            assert entry_id == expected_entry, \
                f'MMU entry ID mismatch: expected 0x{expected_entry:X}, ' \
                f'got 0x{entry_id:X}'

            expected_base_page = (paddr >> 16) + p
            actual_base_page   = entry_val & MMU_PAGE_NUM_MASK
            assert actual_base_page == expected_base_page, \
                f'MMU entry page mismatch: expected 0x{expected_base_page:X}, ' \
                f'got 0x{actual_base_page:X}'

            assert entry_val & MMU_VALID_BIT, \
                f'MMU entry 0x{entry_id:X}: MMU_VALID bit (bit 10) NOT set!'
            assert entry_val & MMU_PSRAM_TYPE_BIT, \
                f'MMU entry 0x{entry_id:X}: MMU_PSRAM_TYPE bit (bit 11) NOT set!'

            unexpected_bits = entry_val & ~(MMU_PAGE_NUM_MASK | MMU_VALID_BIT | MMU_PSRAM_TYPE_BIT)
            assert unexpected_bits == 0, \
                f'MMU entry 0x{entry_id:X}: unexpected bits set in val=0x{entry_val:04X}'

            mmu_entries.append((entry_id, entry_val))

        logging.info(
            f'  ✓ MMU map {m}: vaddr=0x{vaddr:08X} paddr=0x{paddr:08X} '
            f'pages={page_count}'
        )

    assert len(mmu_entries) >= 1, \
        'No MMU entries were programmed — MMU mapping completely failed!'

    dut.expect(r'JMP\[7\] JUMP!', timeout=5)
    logging.info('  ✓ JMP[7]: JUMP — final jump to payload entry point')

    dut.expect(r'JMP\[8\] D-cache drained post-copy', timeout=5)
    logging.info('  ✓ JMP[8]: D-cache drained post-copy (payload bytes in SRAM)')

    # ====================================================================
    # PHASE 7: PAYLOAD ENTRY & "RUG PULL" INTERCEPTORS
    # The payload's shim (stateless_shim) takes over from call_start_cpu0()
    # and --wraps the ESP-IDF early-boot functions so they cannot re-init
    # hardware the loader already configured.
    # ====================================================================
    logging.info('Phase 7: Payload entry & rug-pull interceptors...')

    dut.expect('=== __wrap_call_start_cpu0 ENTERED ===', timeout=5)
    logging.info('  ✓ Shim entry: __wrap_call_start_cpu0')

    # Collect every [Intercepted] ... line. Order is not guaranteed across
    # IDF versions, so scan until the required set is complete or timeout.
    required = {
        'cache_hal_init',
        'mspi_timing_flash_tuning',
        'esp_psram_chip_init',
        'bootloader_flash_update_id',
        'spi_flash_init_chip_state',
        'esp_mspi_pin_init',
        'esp_mspi_pin_reserve',
    }
    interceptor_re = re.compile(r'\[Intercepted\] ([a-z_0-9]+)')
    deadline = time.time() + 30
    while not required.issubset(interceptors_seen) and time.time() < deadline:
        m = dut.expect(interceptor_re, timeout=5)
        interceptors_seen.add(_decode(m.group(1)))

    missing = required - interceptors_seen
    assert not missing, \
        f'Stateless-boot interceptors did NOT fire: {sorted(missing)} ' \
        f'(seen: {sorted(interceptors_seen)})'
    assert len(interceptors_seen) >= MIN_INTERCEPTOR_COUNT, \
        f'Only {len(interceptors_seen)} interceptors fired, expected >= {MIN_INTERCEPTOR_COUNT}'
    logging.info(
        f'  ✓ Rug-pull: {len(interceptors_seen)} interceptors fired '
        f'(required {len(required)}): {sorted(interceptors_seen)}'
    )

    dut.expect('Jumping to esp_startup_start_app...', timeout=10)
    logging.info('  ✓ Handing off to esp_startup_start_app()')

    # ====================================================================
    # PHASE 8: STOCK HELLO-WORLD PAYLOAD EXECUTION & STABILITY
    # ====================================================================
    logging.info('Phase 8: Payload execution & stability...')

    dut.expect(r'main_task: Calling app_main\(\)', timeout=10)
    logging.info('  ✓ app_main() called — FreeRTOS scheduler is running')

    for cycle in range(1, STABILITY_CYCLES + 1):
        # After the previous esp_restart() the full loader chain re-runs
        # (SD mount + Specter verify + anti-phish hash of the 6.8 MB region),
        # so give the payload's banner a generous timeout.
        dut.expect('Hello world!', timeout=30)
        chip_match = dut.expect(
            r'This is esp32p4 chip with (\d+) CPU core\(s\), .*silicon '
            r'revision v(\d+\.\d+), (\d+)MB (?:embedded|external) flash',
            timeout=10
        )
        cores = int(chip_match.group(1))
        silicon_rev = chip_match.group(2)
        flash_mb = int(chip_match.group(3))
        assert cores >= 1 and flash_mb == 8, \
            f'Unexpected chip info: {cores} cores, {flash_mb}MB flash'
        dut.expect(r'Minimum free heap size: (\d+) bytes', timeout=10)
        heap_match = dut.expect(r'Restarting in (\d+) seconds\.\.\.', timeout=10)
        logging.info(
            f'  ✓ Payload cycle {cycle}/{STABILITY_CYCLES}: Hello world!, '
            f'{cores} core, silicon v{silicon_rev}, {flash_mb}MB flash, '
            f'restart in {heap_match.group(1)}s'
        )
        dut.expect('Restarting now.', timeout=15)
        dut.expect(r'rst:0xc\s+\(SW_CPU_RESET\)', timeout=5)

    logging.info(
        f'  ✓ Payload stable across {STABILITY_CYCLES} reboot cycles '
        f'(full loader chain re-runs each time, no WDT resets)'
    )

    # ====================================================================
    # PHASE 9: ANTI-PHISHING WORDS STABILITY ACROSS REBOOTS
    # After the payload's auto-restart cycle, force a hard reset and confirm
    # the SAME 4 BIP-39 words are reported (provisioning is idempotent, the
    # digest is stable).
    # ====================================================================
    logging.info('Phase 9: Anti-phishing words stability across reboot...')

    dut.serial.hard_reset()
    time.sleep(0.5)

    dut.expect('SeedSigner Loader — ESP32-P4 PSRAM payload', timeout=15)
    dut.expect('SD card mounted at /sdcard', timeout=10)
    dut.expect('Signature verification PASSED!', timeout=15)
    reboot_match = dut.expect(
        r'ANTI-PHISHING PROOF:\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)',
        timeout=30
    )
    reboot_words = tuple(_decode(g) for g in reboot_match.groups())
    assert reboot_words == ap_words, \
        f'Anti-phishing words changed after reboot! {ap_words} -> {reboot_words}'
    logging.info(f'  ✓ Words stable after reboot: {" ".join(reboot_words)}')

    # ====================================================================
    # CROSS-PHASE CONSISTENCY CHECKS
    # ====================================================================
    logging.info('Cross-phase consistency checks...')

    entry_in_mmu = False
    for vaddr, paddr, pages in mmu_maps:
        region_start = vaddr
        region_end   = vaddr + pages * MMU_PAGE_SIZE
        if region_start <= entry_addr < region_end:
            entry_in_mmu = True
            break
    assert entry_in_mmu, \
        f'Entry address 0x{entry_addr:08X} does not fall within any MMU-mapped region'
    logging.info(f'  ✓ Entry point 0x{entry_addr:08X} within MMU-mapped region')

    total_mmu_pages = sum(p for _, _, p in mmu_maps)
    expected_pages  = mmu_footprint // MMU_PAGE_SIZE
    assert total_mmu_pages <= expected_pages, \
        f'Total MMU pages ({total_mmu_pages}) exceeds footprint-derived page count ({expected_pages})'
    logging.info(f'  ✓ MMU page budget: {total_mmu_pages} used / {expected_pages} allocated')

    entry_ids = [eid for eid, _ in mmu_entries]
    assert len(entry_ids) == len(set(entry_ids)), 'Duplicate MMU entry IDs detected!'
    logging.info(f'  ✓ MMU entries: {len(mmu_entries)} unique, no duplicates')

    # ====================================================================
    # SUMMARY
    # ====================================================================
    logging.info('')
    logging.info('=' * 64)
    logging.info(' ALL PHASES PASSED — STATELESS LOADER VERIFIED')
    logging.info('=' * 64)
    logging.info(f'  SD bundle:             {loaded_bytes:,} bytes (seedsigner_esp32p4.bin)')
    logging.info(f'  Firmware version:      {pl_ver}')
    logging.info(f'  Anti-phishing words:   {" ".join(ap_words)} (BIP-39 ✓, stable ✓)')
    logging.info(f'  Segments parsed:       {segment_count} '
                 f'({psram_segment_count} PSRAM / {direct_segment_count} direct)')
    logging.info(f'  MMU footprint:         {mmu_footprint:,} bytes ({expected_pages} pages)')
    logging.info(f'  MMU entries:           {len(mmu_entries)} (unique, no duplicates)')
    logging.info(f'  Entry point:           0x{entry_addr:08X} (in MMU region ✓)')
    logging.info(f'  Entry bytes:           0x{entry_bytes:08X} (non-zero ✓)')
    logging.info(f'  Interceptors fired:    {len(interceptors_seen)}')
    logging.info(f'  Payload stability:     {STABILITY_CYCLES} reboot cycles, no WDT resets')
    logging.info(f'  Words after reboot:    {" ".join(reboot_words)} (match ✓)')
    logging.info('=' * 64)
