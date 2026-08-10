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

.PHONY: run clean
run: build/mars
	./build/mars
clean:
	rm -f build/mars build/sh2_recomp.c build/sh2_recomp.o
