MUSASHI = third_party/musashi
NUKED   = third_party/nuked-opn2
GEN     = build/musashi
SDLCF  := $(shell pkg-config --cflags sdl2)
SDLLD  := $(shell pkg-config --libs sdl2)
# -include is what makes src/m68kconf.h effective: Musashi's own sources say
# #include "m68kconf.h", which a quoted include resolves to the copy sitting
# next to them in third_party/, so -Isrc alone never overrode anything. Forcing
# ours in first claims the shared M68KCONF__HEADER guard and the vendored file
# then expands to nothing.
CFLAGS  = -O2 -Wall -Wno-unused-label -include src/m68kconf.h \
          -Isrc -I$(MUSASHI) -I$(NUKED) -I$(GEN) $(SDLCF)

# m68kcpu.c includes m68kfpu.c unconditionally, which needs softfloat even for
# a bare 68000, so softfloat.c is part of the build regardless of CPU type.
SRC = build/sh2_recomp.c build/m68k_recomp.c src/m68000.c \
      src/mem32x.c src/gen68k.c src/genvdp.c src/z80.c src/genz80.c \
      src/psg.c src/sound.c $(NUKED)/ym3438.c src/trace68k.c src/mars_main.c \
      $(MUSASHI)/m68kcpu.c $(GEN)/m68kops.c $(MUSASHI)/softfloat/softfloat.c

build/mars: $(SRC) src/mars.h src/sh2.h src/sound.h src/psg.h src/z80.h src/genz80.h \
            src/m68000.h src/m68kconf.h Makefile $(GEN)/m68kops.c
	clang $(CFLAGS) -o $@ $(SRC) $(SDLLD)

build/sh2_recomp.c:
	python3 tools/recompile.py

build/m68k_recomp.c: build/m68k_cycles.bin
	python3 tools/recompile68k.py

# What each 68000 instruction costs, taken from Musashi rather than from a
# second reading of the manual: the recompiled build sums these per block, so it
# charges what the interpreter charges by construction. Built from Musashi
# alone, which is what keeps it out of the cycle that build/m68k_recomp.c is in.
build/m68k_cycles.bin: tools/m68kcycles.c $(MUSASHI)/m68kcpu.c $(GEN)/m68kops.c
	mkdir -p build
	clang $(CFLAGS) -o build/m68kcycles tools/m68kcycles.c \
	    $(MUSASHI)/m68kcpu.c $(GEN)/m68kops.c \
	    $(MUSASHI)/softfloat/softfloat.c
	./build/m68kcycles > $@

# Musashi generates its opcode tables from a template before it can be built.
$(GEN)/m68kops.c: $(MUSASHI)/m68k_in.c
	mkdir -p $(GEN)
	clang -O2 -o $(GEN)/m68kmake $(MUSASHI)/m68kmake.c
	cd $(GEN) && ./m68kmake . ../../$(MUSASHI)/m68k_in.c

.PHONY: run clean check
run: build/mars
	./build/mars

# Everything that can say "this stopped being true", in the order a break is
# cheapest to understand: the front end reassembles both cartridges byte for
# byte, the recompiler still computes the right answers, and then the runtime
# diffs. Those exit non-zero on a divergence control flow never recovers from
# *and* on a run that stopped short, so a build that crashes early fails here
# rather than passing quietly. The extracts under build/ are rebuilt by the
# tools themselves if missing.
#
# The master is held to its boot rather than the whole extract. That is where
# the signal is, and covering the rest needs a six-million-line trace to compare
# against.
#
# The slave's bound is what our own trace reaches, and the aligner is no longer
# what decides it. Its stream is 93% delay loop, the reference collapses each run
# into one line and we print every step of ours, so the next reference line sits
# thousands of our records away — and `holds` was rejecting any alignment whose
# next line was not within 64. It could not cross a collapsed run at all. Adding
# the run's own length to that reach is what lets it walk.
#
# That briefly looked like a ten-fold lift, to 20,000 blocks, and it was not: the
# slave was banking its share of every hand-over while the adapter still held it
# and bursting through the backlog at hand-over, which carried the walk far past
# where our trace honestly reaches. With that gone the reach is what the trace
# pays for, and roughly linear in it — 2,000,000 lines cross about 2,500 blocks,
# 8,000,000 cross about 7,000. 2,000 is what the budget here covers with margin,
# and it is still the boot, the first PWM interrupts and the sound driver's first
# work. Lifting it is a trace-size decision now, not an aligner one.
#
# The windows are far wider than they were, for one reason: the CPUs now wait
# for each other. A poll that used to be answered inside the 68000's own
# register write is a real wait, so our 68000 spins 65,270 times at 0x881A06
# where it used to spin none, and both SH-2s spend their idle time idling
# instead of being unwound after 64 reads. The window has to be wider than the
# longest such wait or a genuine rejoin reads as a fatal divergence.
#
# The trace budgets survive the idling because both tracers collapse a run of
# one instruction, or one block, in one unchanged state — the reference tracer's
# own `[Omitted: N]`, which tracediff already reads. 3.3 GB of trace became
# 1.35 GB and the master's agreement went up rather than down, because a budget
# now buys game time instead of copies of an idle poll.
#
# The 68000's 6,000,000 is deliberately past what the run produces: the collapse
# is what does the shrinking, and capping on top of it only costs `coverage` the
# breadth it is there to measure — 3,000,000 saves 91 MB and 14 distinct
# addresses, which is the wrong way round.
#
# The SH-2 budget is 2,000,000 because the slave now runs at its measured rate
# of one instruction per cycle rather than one per two, so it produces twice the
# entries in the same game time — and the reference's slave extract is 3.5
# million instructions of mostly idle loop. At 1,000,000 the walk ran out at 62%
# of it, which the diff correctly calls stopping short.
#
# The 68000 round-trips were left out of this for a while, which was the wrong
# call the moment its front end started changing: `coverage` widens discovery and
# only the round-trip can say the widening was right. Both run here now, and the
# last step asks the other half of the question — whether anything the 68000
# actually executed is missing from what discovery found.
#
# The last diff is the recompiled 68000's own gate, and since the translated
# build is now the default the *first* run is the one that has to ask for the
# interpreter by name. `--recomp` used to be held to "same picture, same command
# count" and nothing else, which was tolerable while the interpreter did nine
# tenths of the work and is not now that it does 291 hand-overs in 300 frames.
# It is compared at block granularity because a block label is the only place
# translated code can be hooked — the reference logs every instruction, so
# filtered to those addresses its stream is ours.
#
# `test_recomp68k.py` runs one assembled program through Musashi and through the
# recompiled C over identical memory and compares both the values and the
# condition codes, so the flag rules are held to an independent implementation
# rather than to a reading of the manual.
#
# `diffpwm.py` is the audio's gate and it rides on the same 300-frame run: every
# word the slave pushes into a PWM sample FIFO, against every word the
# reference's slave pushed, in order. The reference's whole extract is a
# constant 0x200 — silence — so what this catches today is our playing something
# where the machine played nothing, which is exactly the failure a sound path
# without a gate produces.
#
# `diffz80.py` is the fourth CPU's, and it holds two things at once that no
# other gate here does: the core, and the *upload*. The Z80's program is not in
# the cartridge in any form a static tool could find — the 68000 assembles it
# into RAM at run time — so a byte wrong in the copy is a wrong instruction, and
# the trace says so on the line it happens. It ends by comparing the bytes that
# reached the YM2612 and the PSG against the reference's own, which is the
# product rather than the process: the driver running right is the means, and
# the same 164 bytes arriving at the two chips in the same order is the end.
#
# `test_psg.py` is there because the trace cannot cover the PSG. This game mutes
# all four of its channels through the whole extract, so nothing the reference
# recorded says whether the core works; what can be checked is the chip's own
# arithmetic — clock / (32 * period) for a tone, two decibels an attenuation
# step, and the shift rates of the noise register — each computed by the test
# rather than copied from the implementation.
check: build/mars
	python3 tools/emit_asm.py --verify
	python3 tools/emit_asm.py --verify --rom "roms/Knuckles' Chaotix (E) [!] (32X).32x"
	python3 tools/disasm68k.py emit --verify
	python3 tools/disasm68k.py --rom "roms/Knuckles' Chaotix (E) [!] (32X).32x" \
	    emit --verify
	python3 tools/test_recomp.py
	python3 tools/test_psg.py
	python3 tools/recompile68k.py --build
	python3 tools/test_recomp68k.py
	./build/mars --interp --frames 300 --trace68k build/trace68k.txt \
	    --trace68k-lines 6000000 --trace-sh2 build/tracesh2.txt \
	    --trace-sh2-lines 2000000 --trace-pwm build/tracepwm.txt \
	    --trace-z80 build/tracez80.txt --trace-z80-lines 2000000 \
	    --dump-z80 build/z80ram.bin --trace-chips build/tracechips.txt >/dev/null
	python3 tools/diff68k.py --ref-lines 20213 --window 400000 --rows 0 --detail 0
	python3 tools/diff68k.py --window 400000 --rows 0 --detail 0
	python3 tools/diffsh2.py --cpu master --window 3000000 --ref-blocks 200 \
	    --rows 0 --detail 0
	python3 tools/diffsh2.py --cpu slave  --window 3000000 --ref-blocks 2000 \
	    --rows 0 --detail 0
	python3 tools/diffpwm.py
	python3 tools/diffz80.py --rows 0 --detail 0
	./build/mars --recomp --frames 300 --trace68k build/trace68k_rc.txt \
	    --trace68k-lines 6000000 >/dev/null
	python3 tools/diff68k.py --blocks --ours build/trace68k_rc.txt \
	    --window 400000 --rows 0 --detail 0
	python3 tools/disasm68k.py coverage
	@echo "all gates pass"

clean:
	rm -f build/mars build/sh2_recomp.c build/sh2_recomp.o
