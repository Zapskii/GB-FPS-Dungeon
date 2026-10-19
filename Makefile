# GB-FPS-Dungeon milestone 1: greybox streaming proof-of-life.
#   make        build dungeon.gb (gbdk-dev Docker image)
#   make clean

ROM    = dungeon.gb
SRCS   = src/main.c src/isr.c src/render.s src/math.s
DEPS   = $(wildcard src/*.h)
RUN    := docker run --rm -u $(shell id -u):$(shell id -g) -v "$(CURDIR)":/work -w /work gbdk-dev
LCC    := /opt/gbdk/bin/lcc

# -Wm-yn : ROM title, -Wl-m/-Wl-j : link map + NoICE symbols (debugging)
CFLAGS = -Wm-yn"FPSDUNGEON" -Wl-m -Wl-j

# make clean && make PROFILE=1 : stage profiler (src/main.c PROF), fixed seed
ifdef PROFILE
CFLAGS += -DPROFILE
endif

$(ROM): $(SRCS) $(DEPS)
	$(RUN) $(LCC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(ROM) dungeon.lst dungeon.sym dungeon.map dungeon.noi dungeon.ihx dungeon.lk

.PHONY: clean