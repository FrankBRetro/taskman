# FLoad for MultiTOS / FreeMiNT (can also be built from the TASKMAN folder: make fload)
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Francis C Bullen
# Needs the m68k-atari-mint cross toolchain (gcc + MiNTLib + gemlib).
#
#   make            68000 build (ST/STE/TT/Falcon)
#   make CPU=-m68020-60
#   make CPU=-mcpu=5475   (FireBee / ColdFire)

CC     = m68k-atari-mint-gcc
STRIP  = m68k-atari-mint-strip
CPU   ?= -m68000
CFLAGS = $(CPU) -O2 -Wall -fomit-frame-pointer
LIBS   = -lgem -lm

fload.app: fload.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
	$(STRIP) $@

clean:
	rm -f fload.app
