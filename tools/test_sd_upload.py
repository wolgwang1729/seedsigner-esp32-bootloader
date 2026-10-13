"""Host-side tests for the SSDU uploader (no hardware required).

Covers the Python framing/client logic plus parity of the dependency-free
device protocol code (uploader/main/ssdu_proto.c), compiled natively with
gcc. Run with:  pytest tools/test_sd_upload.py
"""
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sd_upload import (CHUNK_MAX, MAX_PAYLOAD, OP_BEGIN, OP_COMMIT, OP_HELLO,
                       OP_LIST, OP_REMOVE, OP_VERIFY, OP_WRITE, RESP,
                       STATUS_NAMES, DeviceError, Frames,
                       encode_frame, valid_name)

ROOT = Path(__file__).resolve().parent.parent


def make_resp(status, body):
    return encode_frame(RESP, struct.pack(">I", status) + json.dumps(body).encode())


class FakeDevice:
    """Minimal in-memory SSDU server for Client tests (no serial)."""

    def __init__(self, capacity=2**32):
        self.state = "IDLE"
        self.files = {}
        self.tmp = None
        self.capacity = capacity

    def handle(self, op, payload):
        if op == OP_HELLO:
            return 0, {"state": self.state, "target": "esp32p4",
                       "service": "seedsigner-sd-uploader", "proto": 1,
                       "capacity": self.capacity, "chunk_max": CHUNK_MAX}
        if op == OP_BEGIN:
            nl = payload[0]
            name = payload[1:1 + nl].decode()
            size = int.from_bytes(payload[1 + nl:9 + nl], "big")
            digest = payload[9 + nl:41 + nl]
            if name in self.files:
                return 10, {"state": self.state}
            self.tmp = {"name": name, "size": size, "digest": digest, "buf": bytearray()}
            self.state = "RECEIVING"
            return 0, {"state": self.state, "name": name, "length": size,
                       "sha256": digest.hex(), "offset": 0}
        if op == OP_WRITE:
            off = int.from_bytes(payload[:8], "big")
            crc = int.from_bytes(payload[8:12], "big")
            data = payload[12:]
            assert len(data) == len(payload) - 12
            assert zlib.crc32(data) & 0xFFFFFFFF == crc
            assert off == len(self.tmp["buf"])
            self.tmp["buf"] += data
            return 0, {"state": self.state, "offset": len(self.tmp["buf"])}
        if op == OP_COMMIT:
            assert len(self.tmp["buf"]) == self.tmp["size"]
            got = hashlib.sha256(bytes(self.tmp["buf"])).digest()
            if got != self.tmp["digest"]:
                return 9, {"state": self.state}
            self.files[self.tmp["name"]] = bytes(self.tmp["buf"])
            self.state = "COMMITTED"
            return 0, {"state": self.state, "name": self.tmp["name"],
                       "length": self.tmp["size"], "sha256": got.hex()}
        if op == OP_LIST:
            return 0, {"state": self.state,
                       "entries": [{"name": k, "size": len(v), "dir": False}
                                   for k, v in self.files.items()]}
        if op == OP_VERIFY:
            nl = payload[0]
            name = payload[1:1 + nl].decode()
            size = int.from_bytes(payload[1 + nl:9 + nl], "big")
            digest = payload[9 + nl:41 + nl]
            if name not in self.files:
                return 8, {"state": self.state}
            if len(self.files[name]) != size or \
                    hashlib.sha256(self.files[name]).digest() != digest:
                return 9, {"state": self.state}
            return 0, {"state": self.state, "name": name, "length": size,
                       "sha256": digest.hex()}
        if op == OP_REMOVE:
            nl = payload[0]
            name = payload[1:1 + nl].decode()
            self.files.pop(name, None)
            return 0, {"state": self.state}
        raise AssertionError(f"unexpected op {op}")


class LoopbackTransport:
    """Feeds Client requests straight into FakeDevice (framing-exact)."""

    def __init__(self, device):
        self.device = device
        self.frames = Frames()
        self.pending = bytearray()

    def write(self, data):
        # Re-parse as device-side frames: magic|ver|op|len|payload|crc.
        buf = bytes(data)
        assert buf[:4] == b"SSDU"
        ver, op, ln = struct.unpack(">BBI", buf[4:10])
        assert ver == 1 and len(buf) == 14 + ln
        assert (zlib.crc32(buf[4:-4]) & 0xFFFFFFFF) == int.from_bytes(buf[-4:], "big")
        status, body = self.device.handle(op, buf[10:-4])
        self.pending += make_resp(status, body)

    def read(self):
        out = bytes(self.pending)
        self.pending.clear()
        return out if out else b""


def test_frame_roundtrip():
    # RESP frames roundtrip through the resyncing parser.
    r = make_resp(0, {"ok": True})
    assert Frames().feed(r)[0][1][4:] == b'{"ok": true}'
    # Request frames carry a well-formed envelope (checked field by field).
    f = encode_frame(OP_WRITE, bytes(range(256)) * 32)
    assert f[:4] == b"SSDU"
    ver, op, ln = struct.unpack(">BBI", f[4:10])
    assert (ver, op, ln) == (1, OP_WRITE, 8192)
    assert (zlib.crc32(f[4:-4]) & 0xFFFFFFFF) == int.from_bytes(f[-4:], "big")
    with pytest.raises(ValueError):
        encode_frame(OP_BEGIN, b"x" * (MAX_PAYLOAD + 1))


def test_frame_resync_and_bad_crc():
    good = make_resp(0, {"a": 1})
    fr = Frames()
    assert fr.feed(b"garbage" + good[:5]) == []
    assert len(fr.feed(good[5:] + b"trailing")) == 1
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    assert Frames().feed(bytes(bad)) == []
    huge = b"SSDU\x01\x81" + struct.pack(">I", MAX_PAYLOAD + 1)
    assert Frames().feed(huge + b"\x00" * 20) == []


def test_valid_name():
    assert valid_name("seedsigner_esp32p4.bin")
    assert valid_name("a")
    for bad in ("", ".", "..", "_ssdu_x.part", "a/b", "a\\b", "trail.", "trail ",
                "x" * 129, "has space ok".replace(" ", "|")):
        assert not valid_name(bad), bad
    assert valid_name("has space ok")


def test_status_names_complete():
    assert set(STATUS_NAMES) == set(range(13))


def test_client_upload_verify_list():
    from sd_upload import Client
    dev = FakeDevice()
    client = Client(LoopbackTransport(dev))
    ident = client.hello()
    assert ident["service"] == "seedsigner-sd-uploader"
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fw.bin"
        src.write_bytes(b"SeedSigner-test-payload" * 5000)
        receipt = client.upload(src, "fw.bin", chunk_size=8192, progress=False)
        assert receipt["state"] == "COMMITTED"
        assert receipt["sha256"] == hashlib.sha256(src.read_bytes()).hexdigest()
        back = client.verify(src, "fw.bin")
        assert back["length"] == src.stat().st_size
        page = client.request(OP_LIST, b"")
        assert any(e["name"] == "fw.bin" for e in page["entries"])


def test_upload_skips_when_byte_identical():
    from sd_upload import Client
    dev = FakeDevice()
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fw.bin"
        src.write_bytes(b"payload-v1" * 1000)
        t = LoopbackTransport(dev)
        first = Client(t).upload(src, "fw.bin", 1024, progress=False)
        assert first.get("skipped") is False
        second = Client(LoopbackTransport(dev)).upload(src, "fw.bin", 1024, progress=False)
        assert second.get("skipped") is True
        assert second["sha256"] == hashlib.sha256(src.read_bytes()).hexdigest()
        assert dev.files["fw.bin"] == src.read_bytes()


def test_upload_replaces_when_different():
    from sd_upload import Client
    dev = FakeDevice()
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fw.bin"
        src.write_bytes(b"payload-v1" * 1000)
        Client(LoopbackTransport(dev)).upload(src, "fw.bin", 1024, progress=False)
        src.write_bytes(b"payload-v2!!" * 1000)
        receipt = Client(LoopbackTransport(dev)).upload(src, "fw.bin", 1024, progress=False)
        assert receipt.get("skipped") is False
        assert receipt["sha256"] == hashlib.sha256(src.read_bytes()).hexdigest()
        assert dev.files["fw.bin"] == src.read_bytes()


def test_client_rejects_mismatched_receipt():
    from sd_upload import Client
    dev = FakeDevice()

    orig_commit = FakeDevice.handle

    def lying_commit(self, op, payload):
        if op == OP_COMMIT:
            return 0, {"state": "COMMITTED", "name": "wrong", "length": 1,
                       "sha256": "00" * 32}
        return orig_commit(self, op, payload)

    dev.handle = lying_commit.__get__(dev)
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "fw.bin"
        src.write_bytes(b"abc" * 100)
        with pytest.raises(RuntimeError):
            Client(LoopbackTransport(dev)).upload(src, "fw.bin", 1024, progress=False)


C_KAT = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ssdu_proto.h"
int main(void) {
    /* crc32 must equal zlib (checked against python on the same vectors). */
    assert(ssdu_crc32((const uint8_t *)"", 0) == 0x00000000u);
    assert(ssdu_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    uint8_t d[256]; for (int i = 0; i < 256; i++) d[i] = (uint8_t)i;
    assert(ssdu_crc32(d, 256) == 0x29058C73u);
    /* u64 printer (replaces banned %llu). */
    char b[21];
    assert(ssdu_print_u64(b, sizeof b, 0) == 1 && !strcmp(b, "0"));
    assert(ssdu_print_u64(b, sizeof b, 42) == 2 && !strcmp(b, "42"));
    assert(ssdu_print_u64(b, sizeof b, 15634268160ULL) == 11 && !strcmp(b, "15634268160"));
    assert(ssdu_print_u64(b, sizeof b, 18446744073709551615ULL) == 20 &&
           !strcmp(b, "18446744073709551615"));
    assert(ssdu_print_u64(b, 3, 12345) == 0); /* too small */
    /* frame codec roundtrip incl. CRC. */
    uint8_t out[SSDU_MAX_FRAME];
    const uint8_t pl[] = {1, 2, 3, 4, 5};
    size_t n = ssdu_encode(out, 0x02, pl, sizeof pl);
    assert(n == sizeof pl + 14 && !memcmp(out, "SSDU", 4) && out[4] == 1 && out[5] == 2);
    assert(ssdu_encode(out, 0x02, pl, SSDU_MAX_PAYLOAD + 1) == 0);
    /* feed byte-by-byte with leading noise. */
    uint8_t noisy[SSDU_MAX_FRAME + 8];
    memcpy(noisy, "NOISE!!", 7);
    memcpy(noisy + 7, out, n);
    uint8_t type = 0; const uint8_t *body = 0; size_t blen = 0;
    int64_t t = 1000;
    bool got = false;
    for (size_t i = 0; i < 7 + n; i++)
        if (ssdu_feed(noisy[i], t, &type, &body, &blen)) got = true;
    assert(got && type == 2 && blen == 5 && !memcmp(body, pl, 5));
    /* corrupted CRC must not deliver. */
    out[n - 1] ^= 0xFF;
    got = false;
    for (size_t i = 0; i < n; i++)
        if (ssdu_feed(out[i], t + 3000000, &type, &body, &blen)) got = true;
    assert(!got);
    /* name policy parity with host valid_name(). */
    assert(ssdu_valid_name("seedsigner_esp32p4.bin"));
    assert(!ssdu_valid_name(""));
    assert(!ssdu_valid_name("_ssdu_x.part"));
    assert(!ssdu_valid_name("a/b"));
    assert(!ssdu_valid_name("trail."));
    printf("C_KAT_OK\n");
    return 0;
}
"""


def test_c_proto_parity():
    proto = ROOT / "uploader" / "main" / "ssdu_proto.c"
    header = ROOT / "uploader" / "main" / "ssdu_proto.h"
    assert proto.is_file()
    with tempfile.TemporaryDirectory() as td:
        main_c = Path(td) / "kat_main.c"
        main_c.write_text(C_KAT)
        exe = Path(td) / "kat"
        # SDU_BUFFER must be empty on host (no custom section).
        r = subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-std=c11",
                            "-I", str(header.parent), str(main_c), str(proto),
                            "-o", str(exe)], capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, r.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        assert r.returncode == 0 and "C_KAT_OK" in r.stdout, r.stderr
