#!/usr/bin/env python3
"""SeedSigner SD uploader host tool (SSDU v1).

Pushes a file straight to the board's microSD card over UART without
physically ejecting it — the dev-experience port of specter-bootloader's
`sd_uploader` (see uploader/README.md;
https://github.com/miketlk/specter-bootloader/tree/port_esp32-p4).

Needs only what ESP-IDF already ships: pyserial + esptool (4.12.0).

Line discipline (learned from the reference implementation — getting this
wrong silently resets into the flashed firmware):
  * ONE persistent serial handle spans ROM loading and SSDU talk, within a
    SINGLE process. Never reopen the bridge between `load_ram` execute and
    HELLO: on the CH343 bridge a fresh open/close resets the board and the
    RAM image is lost. There is intentionally no `--no-load` mode: every
    command loads first (~4 s), which is the only robust sequence.
  * HUPCL is cleared so closing never pulses DTR; DTR/RTS are parked
    deasserted on Linux after open.
  * ROM load uses `--no-stub` + `no_reset`/`no_reset`, then the SAME
    descriptor switches to the app baud for SSDU.

Lifecycle:
  1. `esptool load_ram` drops uploader/build/<chip>/seedsigner_sd_uploader.bin
     into internal SRAM and executes it (no flash writes, ROM baud 921600).
  2. This client switches the same port to the app baud (--baud, must equal
     CONFIG_SDU_UART_BAUD) and speaks SSDU framing (SSDU|0x01|type|len|crc32).
  3. Uploads stream as BEGIN + WRITE chunks (CRC-32 each) + COMMIT, which the
     device publishes atomically (.part + SHA-256 temp readback + rename +
     SHA-256 final readback).

Examples:
  %(prog)s --chip esp32p4 --port /dev/ttyACM0 \\
      --uploader-bin uploader/build/esp32p4/seedsigner_sd_uploader.bin \\
      upload seedsigner_esp32p4.bin
  %(prog)s --chip esp32p4 --port /dev/ttyACM0 status
  %(prog)s --chip esp32p4 --port /dev/ttyACM0 list
"""
import argparse
import hashlib
import json
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = b"SSDU"
VERSION = 0x01
RESP = 0x81
MAX_PAYLOAD = 18432
CHUNK_MAX = 16384

OP_HELLO = 0x01
OP_BEGIN = 0x02
OP_WRITE = 0x03
OP_COMMIT = 0x04
OP_ABORT = 0x05
OP_LIST = 0x06
OP_VERIFY = 0x07
OP_REMOVE = 0x08
OP_RELEASE = 0x09

STATUS_NAMES = {
    0: "OK", 1: "BAD_REQUEST", 2: "BAD_PATH", 3: "BAD_RANGE",
    4: "BAD_OFFSET", 5: "NO_MEDIA", 6: "MOUNT_ERROR", 7: "NO_SPACE",
    8: "IO_ERROR", 9: "DIGEST_MISMATCH", 10: "EXISTS", 11: "BUSY",
    12: "UNCERTAIN",
}


class DeviceError(RuntimeError):
    def __init__(self, status, body):
        self.status = status
        self.body = body
        super().__init__(f"device status={status} ({STATUS_NAMES.get(status, '?')}): {body}")


def encode_frame(op, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = struct.pack(">BBI", VERSION, op, len(payload))
    return MAGIC + header + payload + struct.pack(">I", zlib.crc32(header + payload) & 0xFFFFFFFF)


class Frames:
    """Resyncing byte-wise demuxer; returns complete (type, payload) frames."""

    def __init__(self):
        self.buf = bytearray()
        self.since = None

    def feed(self, data, now=None):
        now = time.monotonic() if now is None else now
        out = []
        if self.since is not None and now - self.since > 2:
            self.buf.clear()
            self.since = None
        for byte in data:
            self.buf.append(byte)
            while len(self.buf) >= 4:
                if bytes(self.buf[:4]) != MAGIC:
                    del self.buf[0]
                    self.since = None
                    continue
                if self.since is None:
                    self.since = now
                if len(self.buf) < 10:
                    break
                ver, typ, length = struct.unpack(">BBI", self.buf[4:10])
                if ver != VERSION or typ != RESP or length > MAX_PAYLOAD:
                    del self.buf[0]
                    self.since = None
                    continue
                end = 14 + length
                if len(self.buf) < end:
                    break
                packet = bytes(self.buf[:end])
                if (zlib.crc32(packet[4:-4]) & 0xFFFFFFFF) != int.from_bytes(packet[-4:], "big"):
                    del self.buf[0]
                    self.since = None
                    continue
                out.append((typ, packet[10:-4]))
                del self.buf[:end]
                self.since = None
        return out


def open_session(port):
    """Open the bridge once; the SAME handle serves ROM load and SSDU talk."""
    try:
        import serial
    except ImportError:
        print("ERROR: pyserial not found. Source ESP-IDF export.sh first.", file=sys.stderr)
        raise
    # Open at a standard rate first (macOS rejects tcsetattr after pyserial
    # selects a custom UART rate); the app baud is applied afterwards.
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                        write_timeout=5, exclusive=True)
    ser.dtr = sys.platform == "darwin"
    ser.rts = sys.platform == "darwin"
    ser.port = port
    ser.open()
    try:
        import termios
        attrs = termios.tcgetattr(ser.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(ser.fileno(), termios.TCSANOW, attrs)
    except Exception:
        pass  # non-POSIX hosts keep pyserial defaults
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def load_ram(ser, chip, bin_path, rom_baud=921600):
    """Load the RAM image over the ALREADY-OPEN handle; never closes it."""
    import esptool
    if esptool.__version__ != "4.12.0":
        raise RuntimeError(f"only esptool 4.12.0 qualified (found {esptool.__version__}); "
                           "source ESP-IDF export.sh")
    if chip == "esp32p4":
        from esptool.targets.esp32p4 import ESP32P4ROM as rom_class
    elif chip == "esp32s3":
        from esptool.targets.esp32s3 import ESP32S3ROM as rom_class
    else:
        raise ValueError(f"unsupported chip {chip}")
    print(f"[sd_upload] load_ram {chip} <- {bin_path} @ {rom_baud} baud ...")
    rom = rom_class(ser)
    rom.connect()  # default reset dance into the ROM download mode
    args = ["--chip", chip, "--before", "no_reset", "--after", "no_reset",
            "--no-stub", "--baud", str(rom_baud), "load_ram", str(bin_path)]
    esptool.main(args, esp=rom)  # external esp: port stays open
    # ROM executes the image; the app UART comes up on the same descriptor.
    time.sleep(0.8)


class Transport:
    def __init__(self, ser, baud):
        self.ser = ser
        # Same descriptor, new rate — never reopen (see module docstring).
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.write_timeout = 5
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def write(self, data):
        if self.ser.write(data) != len(data):
            raise OSError("short serial write")
        self.ser.flush()

    def read(self):
        n = self.ser.in_waiting
        return self.ser.read(max(1, min(n if n else 1, 32768)))

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class Client:
    def __init__(self, transport):
        self.t = transport
        self.frames = Frames()

    def request(self, op, payload=b"", timeout=5.0, retries=6):
        frame = encode_frame(op, payload)
        deadline_all = time.monotonic() + timeout * retries
        for attempt in range(retries):
            self.t.write(frame)
            # First HELLO races UART bring-up right after load_ram; retry fast.
            attempt_timeout = min(timeout, 0.3) if (op == OP_HELLO and attempt == 0) else timeout
            deadline = min(deadline_all, time.monotonic() + attempt_timeout)
            while time.monotonic() < deadline:
                for typ, resp in self.frames.feed(self.t.read()):
                    if typ != RESP or len(resp) < 4:
                        continue
                    status = int.from_bytes(resp[:4], "big")
                    try:
                        body = json.loads(resp[4:].decode("utf-8")) if len(resp) > 4 else {}
                    except ValueError:
                        continue
                    if status != 0:
                        raise DeviceError(status, body)
                    return body
        raise TimeoutError(f"op=0x{op:02x}: no RESP after {retries} tries")

    def hello(self):
        body = self.request(OP_HELLO, timeout=8.0)
        if body.get("service") != "seedsigner-sd-uploader":
            raise RuntimeError(f"unexpected HELLO identity: {body}")
        return body

    @staticmethod
    def begin_payload(name, size, digest):
        nb = name.encode("ascii")
        if not 1 <= len(nb) <= 128:
            raise ValueError("bad name length")
        return (struct.pack("B", len(nb)) + nb
                + struct.pack(">Q", size) + digest)

    def upload(self, local_path, sd_name, chunk_size, progress=True):
        data = Path(local_path).read_bytes()
        digest = hashlib.sha256(data).digest()
        if not 1 <= chunk_size <= CHUNK_MAX:
            raise ValueError(f"chunk-size must be 1..{CHUNK_MAX}")
        # Overwrite-unless-identical: never clobber blindly, never re-send
        # bytes the card already holds. Single client, so LIST→VERIFY→REMOVE
        #→BEGIN is race-free in practice; the device still refuses overwrites
        # at BEGIN as a second line of defence.
        listing = self.request(OP_LIST, timeout=10.0).get("entries", [])
        if any(e.get("name") == sd_name and not e.get("directory") for e in listing):
            try:
                self.verify(local_path, sd_name)
            except DeviceError as e:
                if e.status != 9:  # 9 = DIGEST_MISMATCH -> replace below
                    raise
                self.request(OP_REMOVE,
                             struct.pack("B", len(sd_name)) + sd_name.encode("ascii"),
                             timeout=10.0)
            else:
                if progress:
                    print(f"  {sd_name} already up to date (sha256 match), skipping upload")
                return {"state": "COMMITTED", "name": sd_name, "length": len(data),
                        "sha256": digest.hex(), "skipped": True}
        try:
            self.request(OP_BEGIN, self.begin_payload(sd_name, len(data), digest), timeout=10.0)
        except DeviceError as e:
            if e.status == 10:  # EXISTS: never overwrite; say how to proceed.
                raise DeviceError(e.status, {
                    **(e.body if isinstance(e.body, dict) else {}),
                    "hint": f"'{sd_name}' already exists on the SD card and was left "
                            f"untouched. Delete it first with: "
                            f"sd_upload.py --chip <chip> --port <port> remove --name {sd_name}",
                })
            raise
        off = 0
        t0 = time.time()
        while off < len(data):
            chunk = data[off:off + chunk_size]
            payload = struct.pack(">Q", off) + struct.pack(">I", zlib.crc32(chunk) & 0xFFFFFFFF) + chunk
            resp = self.request(OP_WRITE, payload, timeout=10.0)
            if resp.get("offset") != off + len(chunk):
                self.request(OP_ABORT)
                raise RuntimeError(f"offset mismatch: device={resp} local={off + len(chunk)}")
            off += len(chunk)
            if progress:
                pct = 100.0 * off / max(1, len(data))
                print(f"\r  [{off}/{len(data)} bytes, {pct:5.1f}%]", end="", flush=True)
        if progress:
            print(f"  ({time.time() - t0:.1f}s stream)")
        receipt = self.request(OP_COMMIT, timeout=15.0 + len(data) / 32768)
        if (receipt.get("state") != "COMMITTED" or receipt.get("name") != sd_name
                or receipt.get("length") != len(data)
                or receipt.get("sha256") != digest.hex()):
            raise RuntimeError(f"commit receipt mismatch: {receipt}")
        receipt["skipped"] = False
        return receipt

    def verify(self, local_path, sd_name):
        data = Path(local_path).read_bytes()
        digest = hashlib.sha256(data).digest()
        return self.request(OP_VERIFY, self.begin_payload(sd_name, len(data), digest),
                            timeout=15.0 + len(data) / 32768)


def valid_name(name):
    if not isinstance(name, str) or not 1 <= len(name) <= 128:
        return False
    if name in (".", "..") or name.startswith("_ssdu_") or name[-1] in " .":
        return False
    return all(32 <= ord(c) <= 126 and c not in '/\\:"*?<>|' for c in name)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chip", choices=("esp32p4", "esp32s3"), required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600,
                    help="uploader app baud; must equal CONFIG_SDU_UART_BAUD (default 921600)")
    ap.add_argument("--rom-baud", type=int, default=921600,
                    help="baud used for esptool load_ram (default 921600)")
    ap.add_argument("--uploader-bin", type=Path, default=None,
                    help="RAM image to load (default: uploader/build/<chip>/seedsigner_sd_uploader.bin)")
    ap.add_argument("--chunk-size", type=int, default=8192)
    ap.add_argument("--no-release", action="store_true",
                    help="stay mounted after upload (for staging several files per load)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_up = sub.add_parser("upload", help="load RAM image and upload file(s)")
    p_up.add_argument("file", type=Path, nargs="+")
    p_up.add_argument("--name", default=None, help="SD destination basename (single-file uploads only; default: local basename)")
    p_vf = sub.add_parser("verify", help="SHA-256 readback of an SD file against a local file")
    p_vf.add_argument("file", type=Path)
    p_vf.add_argument("--name", required=True)
    sub.add_parser("status", help="HELLO identity + state")
    sub.add_parser("list", help="root directory listing")
    p_rm = sub.add_parser("remove", help="delete an SD file")
    p_rm.add_argument("--name", required=True)
    sub.add_parser("release", help="unmount SD (stays until reset)")
    sub.add_parser("abort", help="abort a half-open upload")
    args = ap.parse_args(argv)

    default_bin = Path(__file__).resolve().parent.parent / "uploader" / "build" / args.chip / "seedsigner_sd_uploader.bin"
    bin_path = args.uploader_bin or default_bin
    if not bin_path.is_file():
        print(f"ERROR: uploader image not found: {bin_path}\n"
              f"Build it with: ./run.sh --target {args.chip} build-uploader", file=sys.stderr)
        return 2

    if args.cmd == "upload":
        pairs = []
        for f in args.file:
            sd_name = args.name if (args.name and len(args.file) == 1) else Path(f).name
            if args.name and len(args.file) > 1:
                print("ERROR: --name only valid for single-file uploads", file=sys.stderr)
                return 2
            if not valid_name(sd_name):
                print(f"ERROR: invalid SD name '{sd_name}'", file=sys.stderr)
                return 2
            pairs.append((Path(f), sd_name))

    ser = open_session(args.port)
    try:
        load_ram(ser, args.chip, bin_path, args.rom_baud)
        with Transport(ser, args.baud) as t:
            c = Client(t)
            ident = c.hello()
            print(f"[sd_upload] HELLO target={ident.get('target')} state={ident.get('state')} "
                  f"cap={ident.get('capacity')} chunk_max={ident.get('chunk_max')}")
            if args.cmd == "status":
                print(json.dumps(ident, indent=2))
            elif args.cmd == "list":
                body = c.request(OP_LIST, timeout=10.0)
                print(json.dumps(body, indent=2))
            elif args.cmd == "upload":
                for local, sd_name in pairs:
                    receipt = c.upload(local, sd_name, args.chunk_size)
                    if receipt.get("skipped"):
                        print(f"[sd_upload] SKIPPED {receipt['name']} "
                              f"(identical file already on card)")
                    else:
                        print(f"[sd_upload] COMMITTED {receipt['name']} "
                              f"({receipt['length']} bytes, sha256={receipt['sha256']})")
                if not args.no_release:
                    rel = c.request(OP_RELEASE)
                    print(f"[sd_upload] RELEASED ({rel.get('state')})")
            elif args.cmd == "verify":
                body = c.verify(args.file, args.name)
                print(f"[sd_upload] VERIFIED {args.name}: {json.dumps(body)}")
            elif args.cmd == "remove":
                body = c.request(OP_REMOVE, struct.pack("B", len(args.name)) + args.name.encode())
                print(f"[sd_upload] REMOVE {args.name}: {json.dumps(body)}")
            elif args.cmd == "release":
                body = c.request(OP_RELEASE)
                print(f"[sd_upload] RELEASED: {json.dumps(body)}")
            elif args.cmd == "abort":
                body = c.request(OP_ABORT)
                print(f"[sd_upload] ABORT: {json.dumps(body)}")
    except DeviceError as e:
        print(f"ERROR: device rejected request: {e}", file=sys.stderr)
        return 1
    except (TimeoutError, OSError, RuntimeError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
