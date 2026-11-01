# GB-FPS-Dungeon milestone 1: greybox streaming proof-of-life.
#   make        build dungeon.gb (gbdk-dev Docker image)
#   make clean

ROM    = dungeon.gb
SRCS   = src/main.c src/isr.c src/render.s src/math.s src/rtabs.s src/billboard.s \
         src/sgb_border.c src/border_data.c
DEPS   = $(wildcard src/*.h)
RUN    := docker run --rm -u $(shell id -u):$(shell id -g) -v "$(CURDIR)":/work -w /work gbdk-dev
LCC    := /opt/gbdk/bin/lcc

# -Wm-yn : ROM title, -Wl-m/-Wl-j : link map + NoICE symbols (debugging)
# -Wm-ys : SGB flag, without it an SGB ignores the border
# -Wm-yt0x03 -Wm-ya1 : MBC1+RAM+BATTERY, one 8 KB SRAM bank (save slot)
CFLAGS = -Wm-yn"FPSDUNGEON" -Wl-m -Wl-j -Wm-ys -Wm-yt0x03 -Wm-ya1

# make clean && make PROFILE=1 : stage profiler (src/main.c PROF), fixed seed
ifdef PROFILE
CFLAGS += -DPROFILE
endif

# make clean && make CHECK=1 : re-read the copied VRAM bank after every
# image, HUD row 1 left shows the mismatching row pairs (should stay 00)
ifdef CHECK
CFLAGS += -DCHECK
endif

# rtabs.s is an absolute area at 0x7800: fail if relocatable ROM overlaps it
$(ROM): $(SRCS) $(DEPS)
	$(RUN) $(LCC) $(CFLAGS) -o $@ $(SRCS)
	@! grep '(REL,CON)' dungeon.map | while read n a z r; do \
	  [ $$((0x$$a)) -lt 32256 ] && [ $$((0x$$a + 0x$$z)) -gt 30720 ] && echo "$$n overlaps rtabs at 0x7800"; \
	done | grep . || { rm -f $@; exit 1; }

# SGB border: art -> GB-SGB-Tooling mkborder.py -> png2asset. Output is
# committed, so only rerun this after changing tools/sgb/stone_art.py.
SGBTOOL ?= $(HOME)/GB-SGB-Tooling
border:
	python3 tools/sgb/stone_art.py tools/sgb/art.png
	python3 $(SGBTOOL)/mkborder.py tools/sgb/border.png --art tools/sgb/art.png
	rm tools/sgb/art.png
	$(RUN) /opt/gbdk/bin/png2asset tools/sgb/border.png -map -bpp 4 -max_palettes 3 \
	  -keep_palette_order -pack_mode sgb -use_map_attributes -c src/border_data.c

clean:
	rm -f $(ROM) dungeon.lst dungeon.sym dungeon.map dungeon.noi dungeon.ihx dungeon.lk

.PHONY: clean border