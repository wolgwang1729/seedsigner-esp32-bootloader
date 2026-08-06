#!/usr/bin/env python3
"""
Generate a vendor payload signing key + the matching vendor_keys[] C snippet.

Usage:
    python3 tools/generate_vendor_key.py [-f]

Writes a fresh ECDSA secp256k1 private key to payload_signing_key.pem (repo
root, gitignored) and prints the `vendor_keys[]` array to paste into
main/main.c. The loader verifies SD-card payload signatures against that
public key, so the C array MUST be updated to match whenever this script
regenerates the key.

Use -f/--force to overwrite an existing key without prompting. Override the
output path with the VENDOR_SIGNING_KEY env var.
"""
import argparse
import os

from ecdsa import SigningKey, SECP256k1

DEFAULT_KEY_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "payload_signing_key.pem"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-f", "--force", action="store_true", help="overwrite existing key without prompting"
    )
    args = parser.parse_args()

    key_path = os.environ.get("VENDOR_SIGNING_KEY", DEFAULT_KEY_PATH)

    if os.path.exists(key_path) and not args.force:
        answer = input(f"{key_path} exists. Overwrite? [y/N] ").strip().lower()
        if answer != "y":
            print("Aborted.")
            return

    priv_key = SigningKey.generate(curve=SECP256k1)
    with open(key_path, "wb") as f:
        f.write(priv_key.to_pem())
    print(f"Wrote {key_path}")

    pub_bytes = priv_key.get_verifying_key().to_string("uncompressed")
    print("const bl_pubkey_t vendor_keys[] = {")
    print("    { .bytes = { " + ", ".join(f"0x{b:02x}" for b in pub_bytes) + " } },")
    print("    BL_PUBKEY_END_OF_LIST\n};")


if __name__ == "__main__":
    main()
