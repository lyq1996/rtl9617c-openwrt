#!/usr/bin/env python3
"""Build the PT1500 RTLFACT1 factory image from already-extracted identity."""
import argparse
import os
import struct

MAGIC = b"RTLFACT1"
BASE_MAC_OFF = 0x1000
PON_MAC_OFF = 0x1010
SERIAL_OFF = 0x1020
MODE_OFF = 0x1030
RADIO0_OFF = 0x2000
RADIO1_OFF = 0x3000
BOSA_OFF = 0x4000
A2H_OFF = 0x5000
A2H_MAGIC = b"PONA2H1\0"
SIZE = 0x6000


def mac_bytes(text):
    parts = text.split(":")
    if len(parts) != 6:
        raise ValueError("MAC must be six colon-separated octets")
    return bytes(int(part, 16) for part in parts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-mac", default="fc:34:e2:77:5c:62")
    parser.add_argument("--pon-mac", default="fc:34:e2:77:5c:63")
    parser.add_argument("--serial", default="59485443E2775C62")
    parser.add_argument("--mode", default="xgpon")
    parser.add_argument("--a2h-dir", default=None,
                        help="directory with a2h_base.bin/a2h_80.bin/a2h_81.bin/a2h_t2.bin")
    parser.add_argument("--output", default="factory.bin")
    args = parser.parse_args()

    serial = args.serial.encode("ascii")
    if len(serial) != 16:
        raise SystemExit("serial must be exactly 16 ASCII bytes")
    mode = args.mode.encode("ascii")
    if len(mode) > 16:
        raise SystemExit("mode must fit in 16 ASCII bytes")
    image = bytearray(b"\xff" * SIZE)
    image[0:8] = MAGIC
    image[BASE_MAC_OFF:BASE_MAC_OFF + 6] = mac_bytes(args.base_mac)
    image[PON_MAC_OFF:PON_MAC_OFF + 6] = mac_bytes(args.pon_mac)
    image[SERIAL_OFF:SERIAL_OFF + 16] = serial
    image[MODE_OFF:MODE_OFF + len(mode)] = mode
    if args.a2h_dir:
        pages = ["a2h_base.bin", "a2h_80.bin", "a2h_81.bin", "a2h_t2.bin"]
        archive = bytearray(A2H_MAGIC)
        for name in pages:
            with open(os.path.join(args.a2h_dir, name), "rb") as handle:
                page = handle.read(128)
            if len(page) != 128:
                raise SystemExit(f"{name}: expected 128 bytes")
            archive += page
        image[A2H_OFF:A2H_OFF + len(archive)] = archive
    # Radio calibration and BOSA stay 0xff: factory_blob() then skips them.
    with open(args.output, "wb") as handle:
        handle.write(image)
    print(f"{args.output}: {len(image)} bytes, magic={MAGIC.decode()}, "
          f"base={args.base_mac}, pon={args.pon_mac}, serial={args.serial}")


if __name__ == "__main__":
    main()
