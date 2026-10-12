# Writes a ROM out as a C header for the netplay tests.
# SPDX-License-Identifier: MIT
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
lines = []
for i in range(0, len(data), 12):
    lines.append("\t" + " ".join("0x%02X," % b for b in data[i:i + 12]))
with open(dst, "w") as f:
    f.write("/* Generated from src/feature/netplay/test/rom by embed.py; do not edit.\n")
    f.write(" * linktest.gba is MIT licensed, see src/feature/netplay/test/rom. */\n")
    f.write("static const unsigned char linktestRom[] = {\n")
    f.write("\n".join(lines))
    f.write("\n};\n")
