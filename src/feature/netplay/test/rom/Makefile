# Builds linktest.gba and regenerates the header the tests embed.
# SPDX-License-Identifier: MIT
#
# Only needed when changing the ROM; the built ROM and header are checked in.
# Requires arm-none-eabi-gcc (Debian/Ubuntu: gcc-arm-none-eabi) and python3.

PREFIX ?= arm-none-eabi-
CC := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
CFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding -fno-common -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -T linktest.ld -Wl,--gc-sections

HEADER := ../linktest-rom.h

all: linktest.gba $(HEADER)

linktest.elf: crt0.s linktest.c linktest.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.s linktest.c

linktest.gba: linktest.elf
	$(OBJCOPY) -O binary $< $@
	python3 fixheader.py $@

$(HEADER): linktest.gba
	python3 embed.py $< $@

clean:
	rm -f linktest.elf

.PHONY: all clean
