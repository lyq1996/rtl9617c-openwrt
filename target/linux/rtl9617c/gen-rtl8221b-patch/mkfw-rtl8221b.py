#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import struct
import sys

SECTIONS = ("ram", "patch", "post")


def parse(path):
    regs = {name: [] for name in SECTIONS}
    current = None
    with open(path) as f:
        for number, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("["):
                current = line.strip("[]")
                if current not in regs:
                    sys.exit(f"{path}:{number}: unknown section {current}")
                continue
            if current is None:
                sys.exit(f"{path}:{number}: register outside a section")
            reg, val = (int(x, 0) for x in line.split())
            if reg > 0xffff or val > 0xffff:
                sys.exit(f"{path}:{number}: value out of range")
            regs[current].append((reg, val))
    return regs


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <source.txt> <output.bin>")
    regs = parse(sys.argv[1])
    data = struct.pack("<8sH3H", b"RTL8221B", 1,
                       *(len(regs[name]) for name in SECTIONS))
    for name in SECTIONS:
        for reg, val in regs[name]:
            data += struct.pack("<HH", reg, val)
    with open(sys.argv[2], "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
