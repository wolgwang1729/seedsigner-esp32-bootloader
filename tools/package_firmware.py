#!/usr/bin/env python3
import struct
import zlib
import sys
import os

BL_SECT_MAGIC = 0x54434553
BL_SECT_STRUCT_REV = 1

def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF

def pack_attribute(key, value):
    if isinstance(value, str):
        value = value.encode('ascii')
    elif isinstance(value, int):
        # Determine size needed
        if value < 256:
            value = struct.pack('<B', value)
        elif value < 65536:
            value = struct.pack('<H', value)
        else:
            value = struct.pack('<I', value)
    
    return struct.pack('<BB', key, len(value)) + value

def build_section(name, version, payload, attributes=b''):
    # Pad attributes to 216 bytes
    attr_list = attributes + b'\x00' * (216 - len(attributes))
    if len(attr_list) > 216:
        raise ValueError("Attributes too large")
        
    pl_size = len(payload)
    pl_crc = crc32(payload) if pl_size > 0 else 0
    
    # Pack header without struct_crc
    # magic(4), rev(4), name(16), ver(4), size(4), crc(4), attrs(216) = 252 bytes
    fmt = '<II16sIII216s'
    header = struct.pack(fmt,
                         BL_SECT_MAGIC,
                         BL_SECT_STRUCT_REV,
                         name.encode('ascii'),
                         version,
                         pl_size,
                         pl_crc,
                         attr_list)
                         
    struct_crc = crc32(header)
    header += struct.pack('<I', struct_crc)
    
    return header + payload

def main():
    if len(sys.argv) < 3:
        print("Usage: package_firmware.py <input.bin> <output.bin>")
        sys.exit(1)
        
    in_file = sys.argv[1]
    out_file = sys.argv[2]
    
    with open(in_file, 'rb') as f:
        payload = f.read()
        
    # Main firmware section
    # bl_attr_platform = 4, bl_attr_algorithm = 1
    attrs = pack_attribute(4, "seedsigner_esp32p4") + pack_attribute(1, "secp256k1-sha256")
    main_section = build_section("main", 1, payload, attrs)
    
    # Signature section (dummy 64-byte payload)
    # The actual Specter bootloader expects a specific structure for signature
    # payload, but a random 64-byte payload will just fail validation cleanly.
    dummy_sig = os.urandom(64)
    sig_section = build_section("sign", 1, dummy_sig)
    
    with open(out_file, 'wb') as f:
        f.write(main_section)
        f.write(sig_section)
        
    print(f"Packaged firmware: {out_file}")

if __name__ == "__main__":
    main()
