MUSASHI = third_party/musashi
GEN     = build/musashi
SDLCF  := $(shell pkg-config --cflags sdl2)
SDLLD  := $(shell pkg-config --libs sdl2)
# -include is what makes src/m68kconf.h effective: Musashi's own sources say
# #include "m68kconf.h", which a quoted include resolves to the copy sitting
# next to them in third_party/, so -Isrc alone never overrode anything. Forcing
# ours in first claims the shared M68KCONF__HEADER guard and the vendored file
# then expands to nothing.
CFLAGS  = -O2 -Wall -Wno-unused-label -include src/m68kconf.h \
          -Isrc -I$(MUSASHI) -I$(GEN) $(SDLCF)

# m68kcpu.c includes m68kfpu.c unconditionally, which needs softfloat even for
# a bare 68000, so softfloat.c is part of the build regardless of CPU type.
SRC = build/sh2_recomp.c src/mem32x.c src/gen68k.c src/genvdp.c src/trace68k.c src/mars_main.c \
      $(MUSASHI)/m68kcpu.c $(GEN)/m68kops.c $(MUSASHI)/softfloat/softfloat.c

build/mars: $(SRC) src/mars.h src/sh2.h src/m68kconf.h Makefile $(GEN)/m68kops.c
	clang $(CFLAGS) -o $@ $(SRC) $(SDLLD)

build/sh2_recomp.c:
	python3 tools/recompile.py

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
# the signal is — 158 of 177 blocks in lock step, against 176 of 23,077 further
# out, where our trip counts through the poll loops diverge by design — and
# covering the rest needs a six-million-line trace to compare against.
#
# The 68000 round-trips were left out of this for a while, which was the wrong
# call the moment its front end started changing: `coverage` widens discovery and
# only the round-trip can say the widening was right. Both run here now, and the
# last step asks the other half of the question — whether anything the 68000
# actually executed is missing from what discovery found.
check: build/mars
	python3 tools/emit_asm.py --verify
	python3 tools/emit_asm.py --verify --rom "roms/Knuckles' Chaotix (E) [!] (32X).32x"
	python3 tools/disasm68k.py emit --verify
	python3 tools/disasm68k.py --rom "roms/Knuckles' Chaotix (E) [!] (32X).32x" \
	    emit --verify
	python3 tools/test_recomp.py
	./build/mars --frames 300 --trace68k build/trace68k.txt \
	    --trace68k-lines 2000000 --trace-sh2 build/tracesh2.txt >/dev/null
	python3 tools/diff68k.py --ref-lines 20213 --rows 0 --detail 0
	python3 tools/diff68k.py --rows 0 --detail 0
	python3 tools/diffsh2.py --cpu master --window 300000 --ref-blocks 200 \
	    --rows 0 --detail 0
	python3 tools/diffsh2.py --cpu slave  --window 300000 --rows 0 --detail 0
	python3 tools/disasm68k.py coverage
	@echo "all gates pass"

clean:
	rm -f build/mars build/sh2_recomp.c build/sh2_recomp.o
