MUSASHI = third_party/musashi
GEN     = build/musashi
SDLCF  := $(shell pkg-config --cflags sdl2)
SDLLD  := $(shell pkg-config --libs sdl2)
CFLAGS  = -O2 -Wall -Wno-unused-label -Isrc -I$(MUSASHI) -I$(GEN) $(SDLCF)

# m68kcpu.c includes m68kfpu.c unconditionally, which needs softfloat even for
# a bare 68000, so softfloat.c is part of the build regardless of CPU type.
SRC = build/sh2_recomp.c src/mem32x.c src/gen68k.c src/mars_main.c \
      $(MUSASHI)/m68kcpu.c $(GEN)/m68kops.c $(MUSASHI)/softfloat/softfloat.c

build/mars: $(SRC) src/mars.h src/sh2.h $(GEN)/m68kops.c
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
