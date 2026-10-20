# GB-FPS-Dungeon milestone 1: greybox streaming proof-of-life.
#   make        build dungeon.gb (gbdk-dev Docker image)
#   make clean

ROM    = dungeon.gb
SRCS   = src/main.c src/isr.c src/render.s src/math.s src/rtabs.s
DEPS   = $(wildcard src/*.h)
RUN    := docker run --rm -u $(shell id -u):$(shell id -g) -v "$(CURDIR)":/work -w /work gbdk-dev
LCC    := /opt/gbdk/bin/lcc

# -Wm-yn : ROM title, -Wl-m/-Wl-j : link map + NoICE symbols (debugging)
CFLAGS = -Wm-yn"FPSDUNGEON" -Wl-m -Wl-j

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

clean:
	rm -f $(ROM) dungeon.lst dungeon.sym dungeon.map dungeon.noi dungeon.ihx dungeon.lk

.PHONY: clean