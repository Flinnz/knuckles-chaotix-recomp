CFLAGS = -O2 -Wall -Wno-unused-label -Isrc

build/mars: build/sh2_recomp.c src/mem32x.c src/mars_main.c src/mars.h src/sh2.h
	clang $(CFLAGS) -o $@ build/sh2_recomp.c src/mem32x.c src/mars_main.c

build/sh2_recomp.c:
	python3 tools/recompile.py

.PHONY: run clean
run: build/mars
	./build/mars
clean:
	rm -f build/mars build/sh2_recomp.c build/sh2_recomp.o
