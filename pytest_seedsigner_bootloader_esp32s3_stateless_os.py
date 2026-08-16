# SPDX-FileCopyrightText: 2024-2026
# SPDX-License-Identifier: CC0-1.0
#
# Pytest integration test for the SeedSigner Stateless Bootloader (ESP32-S3)
#
# Usage:
#   source ~/esp/esp-idf-v5.5/export.sh
#   pytest -s --embedded-services esp,idf --target esp32s3 \
#          --port /dev/ttyACM0 pytest_seedsigner_bootloader_esp32s3_stateless_os.py
#
# Architecture reference (ESP32-S3 memory map):
#   Internal SRAM (IRAM):       0x40370000 - 0x403E0000  (instruction side)
#   Internal SRAM (DRAM):       0x3FC88000 - 0x3FCF0000  (data side, alias +0x6F0000)
#   Flash cache (IROM window):  0x42000000 - 0x44000000  (shared I/D MMU table)
#   Flash/PSRAM (DROM window):  0x3C000000 - 0x3E000000  (shared I/D MMU table)
#   MMU page size:              64 KB (0x10000)
#   Loader JMP zone (IRAM):     0x403A0000 - 0x403B8000  (must not be hit by payload)
# ---------------------------------------------------------------------------

import logging
import os
import re
import time

import pytest
from pytest_embedded_idf.app import IdfApp
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize

# ── ESP32-S3 memory map constants ──────────────────────────────────────────
IROM_VADDR_START    = 0x42000000
IROM_VADDR_END      = 0x44000000
DROM_VADDR_START    = 0x3C000000
DROM_VADDR_END      = 0x3E000000
IRAM_VADDR_START    = 0x40370000
IRAM_VADDR_END      = 0x403E0000
JMP_ZONE_START      = 0x403A0000
JMP_ZONE_END        = 0x403B8000
MMU_PAGE_SIZE       = 0x10000       # 64 KB
MMU_LINEAR_ADDR_MASK = 0x1FFFFFF    # 25-bit linear address

# Image format constants
MAX_FIRMWARE_SIZE   = 8 * 1024 * 1024
MAX_SEGMENT_COUNT   = 16

# Timeout for the anti-phishing proof line.
ANTI_PHISH_TIMEOUT  = 300

# Payload reboot cycles
STABILITY_CYCLES = 1


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


@pytest.mark.generic
@idf_parametrize('target', ['esp32s3'], indirect=['target'])
def test_stateless_bootloader_and_payload(app: IdfApp, dut: IdfDut) -> None:
    """
    End-to-end integration test for the SeedSigner stateless bootloader (S3).
    """
    def _in_irom(a): return IROM_VADDR_START <= a < IROM_VADDR_END
    def _in_drom(a): return DROM_VADDR_START <= a < DROM_VADDR_END
    def _in_psram_window(a): return _in_irom(a) or _in_drom(a)

    segments = []
    ap_words = None

    # ====================================================================
    # PHASE 1: LOADER START & PURE SD-CARD PAYLOAD ACQUISITION
    # ====================================================================
    logging.info('Phase 1: Pure SD-card payload acquisition...')

    load_match = dut.expect(
        r'Loaded (\d+) bytes from /sdcard/seedsigner_esp32s3\.bin',
        timeout=20
    )
    
    loaded_bytes = int(load_match.group(1))
    assert 0 < loaded_bytes <= MAX_FIRMWARE_SIZE, \
        f'Loaded {loaded_bytes} bytes out of valid range (0, {MAX_FIRMWARE_SIZE}]'
    
    logging.info(f'  ✓ Loaded {loaded_bytes:,} bytes from SD card (/sdcard/seedsigner_esp32s3.bin)')
    logging.info('Phase 2: Specter multisig verification...')
    dut.expect('Specter bootloader section detected', timeout=10)
    ver_match = dut.expect(r'Firmware version:\s+(\d+)', timeout=5)
    pl_ver = int(ver_match.group(1))
    assert pl_ver >= 1, f'Firmware version {pl_ver} fails downgrade check'
    dut.expect('Performing secp256k1 multisig verification...', timeout=5)
    dut.expect('Signature verification PASSED!', timeout=10)
    logging.info('  ✓ secp256k1 multisig signature verified')

    # ====================================================================
    # PHASE 3: ANTI-PHISHING PROOF
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
    assert IRAM_VADDR_START <= entry_addr < IRAM_VADDR_END, \
        f'Entry address 0x{entry_addr:08X} is not in the S3 IRAM window'
    assert not (JMP_ZONE_START <= entry_addr < JMP_ZONE_END), \
        f'Entry address 0x{entry_addr:08X} is inside the loader JMP zone!'
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
        assert not (seg_addr < JMP_ZONE_END and seg_addr + seg_len > JMP_ZONE_START), \
            f'Segment {i} at 0x{seg_addr:08X} collides with the loader JMP zone!'

        if _in_psram_window(seg_addr):
            route_match = dut.expect(
                r'->\s+fake_flash\+0x([0-9A-Fa-f]+)',
                timeout=5
            )
            offset = int(route_match.group(1), 16)
            assert (seg_addr & MMU_LINEAR_ADDR_MASK) == offset, \
                f'Segment {i}: fake_flash offset 0x{offset:X} != linear addr ' \
                f'0x{seg_addr & MMU_LINEAR_ADDR_MASK:X}'
            route = 'fake_flash'
            psram_segment_count += 1
        else:
            route_match = dut.expect(
                r'->\s+direct copy to 0x([0-9A-Fa-f]+)',
                timeout=5
            )
            dest = int(route_match.group(1), 16)
            assert dest == seg_addr, \
                f'Segment {i}: direct-copy dest 0x{dest:X} != segment addr 0x{seg_addr:X}'
            route = 'direct copy'
            direct_segment_count += 1

        segments.append((seg_addr, seg_len, route))
        logging.info(
            f'  ✓ Seg {i}: addr=0x{seg_addr:08X} len={seg_len:,} -> {route}'
        )

    assert psram_segment_count >= 1, \
        'No PSRAM-window segments found — payload has no PSRAM XIP code'
    logging.info(
        f'  ✓ Routing: {psram_segment_count} PSRAM (fake_flash), '
        f'{direct_segment_count} direct (SRAM)'
    )

    # ====================================================================
    # PHASE 6: BARE-METAL JUMP SEQUENCE & BOOT
    # ====================================================================
    logging.info('Phase 6: Bare-metal jump sequence...')

    dut.expect('Ready to boot payload', timeout=10)
    logging.info('  ✓ Ready to boot payload and jumped')
