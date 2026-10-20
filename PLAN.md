# Plan after build Y (speed round 4)

## Where things stand (2026-10-20)

- Build V committed (282f173) and confirmed on a DMG: floors 1-4 clean,
  `make CHECK=1` stayed 00 while turning/walking/map.
- Build Y committed (add3fa0): Wave 1 (billboard.s, ai_step asm, copy
  handover) and Wave 2 S5 (odd-column walk) are in. Start view in
  gearboy, `make PROFILE=1`: 4.08 -> ~4.00 frames per image (~15 fps).
  Lines per image now: render ~308, copy ~199, logic ~46,
  sprite_update ~49, rest ~4. **Not yet run on a DMG.**
- Wave 2 S4 (ragged rows merged in fill) was implemented, measured
  -3,038 T/view on 400 random views, and **reverted**: +6.3 lines/image
  at the gearboy start view (near walls make almost every row ragged).
- HUD marker: every single letter is used (V and Y were the last). The
  next build must add a glyph to the font in main.c / the font table.
- Test tools: `tools/sim/` (see its README). Setup:
  `python3 -m venv tools/sim/venv && tools/sim/venv/bin/pip install pyboy`.

## Measurement lessons (2026-10-20)

- The gearboy profiler counts scanlines (456 T) and its last image is
  partial: use **64 images** (PROF_N), never ~20, or the per-image
  average drifts by ~6%.
- A render change must be judged at the **start view**, not on the
  400-view bench mean (S4 is the counter-example). Ask agents for the
  per-view spread.
- Write a baseline snapshot of `src/` before handing out work; agents'
  worktrees start at HEAD, not at the uncommitted working tree.

## Rules

- **One file per agent per wave.** The main session integrates, builds,
  runs the checks, measures in gearboy, bumps the HUD marker (none
  left: add a font glyph) and updates the README. Commit or push only
  when the user says so. Snapshot `src/` as the baseline first; agent
  worktrees start at HEAD, not at uncommitted changes.
- Agents get their brief from the templates below and **return a diff
  summary and their own check results**, not a commit.
- Code style: match render.s (lowercase mnemonics, `;` comments at column
  40, a short header comment per routine, `ponytail:` comments only for
  deliberate corner cuts). sdasgb gotchas are in README "Gotchas".
- Code lookups: codebase-memory graph tools first (project
  `Users-simonblythe-GB-FPS-Dungeon`); `.s` files and `src/ai.h` only
  partly parse, so grep those too.

## Gates (every change)

| Change | Gate |
|---|---|
| Meant to keep the image | `bench.py --cmp base.pkl new.pkl` -> `0/N views differ` |
| Touches copy_view / phase 2 | `copysim.py src 60`: no `viol_`, no `_mismatch`, burst store slack >= 40 T |
| Touches C game logic | game plays in gearboy (start, turn, walk, shoot, map, next floor) |
| Any display path | `make CHECK=1` in gearboy shows 00 while turning/walking/map |
| Speed claim | gearboy `make PROFILE=1` start view, 64 images (procedure below) |

Baseline for `--cmp`: `git archive <ref> src | tar -x -C tools/sim/base`
(HEAD after build V is committed; before that use the working tree copy
taken at the start of the wave).

Gearboy profile procedure: `make clean && make PROFILE=1`; `_prof_acc`
address from dungeon.noi (WRAM area 6 offset = addr - 0xC000); load ROM,
debug_continue, press/release start twice (title, then game), wait ~4 s
for the banner, write 29 zero bytes at `_prof_acc` (**write_memory pauses
the emulator: always debug_continue afterwards**), continue, wait until
the last byte reads 0x40 (64 images; at ~1 image/s that is a minute),
read 29 bytes: 7 little-endian uint32 scanline totals (logic, render,
seen, flip wait, copy, sprite_update, rest) over 64 images; /64 = lines
per image, /154 = frames. A read taken before 64 images includes a
partial image and reads ~6% high.

## Wave 0 (main session, with the user) — done

1. Ask the user to run floors 3-4 and the `make CHECK=1` build on the
   DMG. If anything shows (stray pixels, CHECK > 00), stop and debug the
   copy first: `copysim.py` with a heavier sprite model, and compare
   against `copy_vbuf` (the old polled copy, still in render.s) by
   swapping the call in main.c.
2. Commit build V when the user agrees (one commit, README already has
   the entry), then take the `--cmp` baseline from HEAD.

## Wave 1 (parallel, 3 agents, one file each) — done in build Y

**S1 `src/render.s`: chain copy pieces** (est. 15-25 copy lines)
- Today each piece end (`cw_tail`, `cw_3l`/`cw_6l`, `cu_5l`/`cu_9l`,
  `cu_tail` -> `cw_segend` -> `cw_next` -> `cw_sync`) waits for the next
  mode 2, usually losing a line; ~25 pieces per copy.
- Within a column the pieces are contiguous in VRAM (V [T, core), U core,
  V [core end, B)), so a transition only swaps the source: make the
  piece-ending burst hand over to the next piece's first burst through
  the chain path instead of a sync, when cw_next's work fits before the
  next mode-0 request (budget rules in the copy_view header comment).
- Late halts are tolerated (the first instruction after every halt must
  stay idempotent), but stores must keep >= 40 T slack in copysim.

**S2 `src/ai.h`: enemy step in asm** (est. ~5-8k T per AI tick)
- `ai_step` (C) costs ~1.2k T per enemy, mostly SDCC 16-bit array and
  global arithmetic; `ai.h` already holds an asm BFS (`ai_bfs`) to follow
  for style and calling convention (sdcccall(1)).
- Keep behaviour identical: write a host-side or interpreter A/B
  (`prof.py ... <hex entry>` with `DEBUG=1`, or a small harness like
  `tools/sim/bench.inc`) comparing ex/ey after N ticks against the C
  version for random mazes and positions.

**S3 new `src/billboard.s`: draw_art and the OAM writes** (est. ~8-10k T)
- `sprite_update` spends ~10k T outside `project()`, mostly `draw_art`
  (GBDK set_sprite_tile/prop + move_sprite per tile). Write `draw_art`
  in asm writing `shadow_OAM` directly; same signature and return value.
- The main session wires it in (main.c, Makefile SRCS) and checks OAM
  with `bench.py --cmp` (OAM must match on every view; the bench views
  rarely show sprites, so also add an interpreter A/B over random
  art/x/y/pal inputs, like the project() test).

## Wave 2 (render.s, one at a time) — S5 done in build Y; S4 rejected

**S4 ragged edges in fill** (est. ~5-8k T) — implemented, reverted:
-3,038 T/view on 400 random views but +6.3 lines/image at the start
view. Only worth revisiting with a cheap "no ragged rows" fast path or a
version tuned for many ragged rows.
- `f_rag` (RTOP/RBOT macros) read-modify-writes each ray's rows above
  maxtop / below minbot: ~14.6k T per view. Write rows [mintop, maxtop)
  and [minbot, maxbot) once each, combining the ceiling/floor pattern
  with the wall colour of the rays that cover the row. Byte-identical.

**S5 odd-column pass** (est. ~4-6k T) — done: od_loop 15,170 -> 11,884 T,
-3,519 T/view, every bench view faster; start view render 319 -> 308
lines/image.
- `od_loop` ~15k T per view: one pointer walk instead of per-array
  address math (`e2_loop` does the same for k±2). Byte-identical.

## Wave 3 (features, only once the user picks)

Ideas offered: 2-colour wall textures on near walls (`textures.h` is
dead), a ranged enemy or a floor-4 boss, MBC5 for saves/more floors
(~8.6 KB ROM left), difficulty and score screen, footstep/alert sounds.

## Agent brief template

```
You are working on GB-FPS-Dungeon (Game Boy DMG raycaster, GBDK-2020 in
the gbdk-dev Docker image, `make` / `make PROFILE=1` / `make CHECK=1`).
Read PLAN.md (task <ID>), README "Constraints & next steps" (Build V) and
"Gotchas", and the header comments of the routines you touch.
You may edit ONLY <file>. Do not commit. Do not touch other files; if
another file must change, describe the change in your report.
Goal: <goal from PLAN.md>. Keep behaviour/image <identical | as stated>.
Checks you must run (tools/sim/README.md; venv at tools/sim/venv):
<gates from the table>. Report: what changed and why, measured cycles
before/after (prof.py or copysim.py), gate results with the exact output
lines, and anything left unverified.
```
