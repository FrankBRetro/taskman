# TaskMan, FLoad and FPulse for MultiTOS / FreeMiNT
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Francis C Bullen
# Needs the m68k-atari-mint cross toolchain (gcc + MiNTLib + gemlib).
#
#   make            build all three (68000: ST/STE/TT/Falcon)
#   make taskman    build TaskMan only
#   make fload      build FLoad only (in FLOAD/)
#   make fpulse     build FPulse only (in FPULSE/)
#   make CPU=-m68020-60
#   make CPU=-mcpu=5475   (FireBee / ColdFire)

CC     = m68k-atari-mint-gcc
STRIP  = m68k-atari-mint-strip
CPU   ?= -m68000
CFLAGS = $(CPU) -O2 -Wall -fomit-frame-pointer

all: taskman fload fpulse

taskman: taskman.app
fload: FLOAD/fload.app
fpulse: FPULSE/fpulse.app

taskman.app: taskman.c
	$(CC) $(CFLAGS) -o $@ $< -lgem
	$(STRIP) $@

FLOAD/fload.app: FLOAD/fload.c
	$(CC) $(CFLAGS) -o $@ $< -lgem -lm
	$(STRIP) $@

FPULSE/fpulse.app: FPULSE/fpulse.c
	$(CC) $(CFLAGS) -o $@ $< -lgem -lm
	$(STRIP) $@

clean:
	rm -f taskman.app FLOAD/fload.app FPULSE/fpulse.app

.PHONY: all taskman fload fpulse clean
