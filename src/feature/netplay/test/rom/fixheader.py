# Pads a GBA ROM to a 4-byte multiple and fixes its header complement check.
# SPDX-License-Identifier: MIT
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    rom = bytearray(f.read())
while len(rom) % 4:
    rom.append(0)
check = (-sum(rom[0xA0:0xBD]) - 0x19) & 0xFF
rom[0xBD] = check
with open(path, "wb") as f:
    f.write(rom)
