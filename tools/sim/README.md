# Test tools for render.s / copy_view (not part of the build)

Setup once: `python3 -m venv tools/sim/venv && tools/sim/venv/bin/pip install pyboy`.
Each tool builds a scratch copy of a `src/` dir with `bench.inc` (a bench
`main()`) in `tools/sim/bw/` through the gbdk-dev Docker image. `P=tools/sim/venv/bin/python`.

| Tool | Use |
|---|---|
| `bench.py SRC OUT.pkl [N]` | render N random views in PyBoy, dump outputs, rough T (timer) |
| `bench.py SRC OUT.pkl N --motion [K]` | each random view followed by K still/turn/walk/strafe views (default 8), so consecutive images share columns: the test for copy_view's column skip (`c_key`), which random views almost never trigger |
| `bench.py --cmp A.pkl B.pkl` | compare two dumps: ray arrays, seen[], OAM, and the two copied VRAM banks (build V no longer writes every vbuf row, so comparing against vbuf gives false differences) |
| `prof.py SRC [N] [ENTRY]` | exact cycles per label/instruction of an asm routine (SM83 interpreter `sm83.py`); `DEBUG=1 BYLINE=25` for C lines, ENTRY may be a hex address from a `-debug` .noi |
| `copysim.py SRC [N]` | copy_view cycle by cycle against a PPU model (`ppusim.py`, mode 3 172-289 T): VRAM writes in mode 3, store slack, late halts, lines per copy. The bench snapshot is taken after the real copy, so it restores r_vh, c_key (from bench.inc's b_key) and the c_desc of columns that copy skipped; full mode invalidates the keys like view_reset. Spans mode can't catch a stale skip: use `bench.py --motion` + `--cmp` and CHECK |
| `pickup_test.py [DIR]` | builds DIR/src (default: the repo) and plays it in PyBoy: a key is taken, a health item is left at full hearts and taken once hurt |
| `enemy_ab.py OLD NEW [N]` | enemy_tick() of two src dirs in the SM83 interpreter from the same random enemy states (alive, stun, type, positions near the camera, dmg_cd): compares positions, stuns, hp, flashes, ai_dist |
| `cull_check.py [N]` | project()'s per-image cull radius (`tools/gen_cull.py`) never drops an object a model of project() calls visible (random objects over bench views) |

Baseline for A/B: `mkdir -p tools/sim/base && git archive HEAD src | tar -x -C tools/sim/base`,
then `$P tools/sim/bench.py tools/sim/base/src tools/sim/base.pkl 400`.
Run them from the repo root. PyBoy has no VRAM blocking: copy timing is only
trustworthy in copysim.py, mGBA or on a DMG (`make CHECK=1`).

Bench means are not the whole story: a change can win on 400 random views and
lose at the start view (the speed gate). Before integrating a render change,
check the per-view spread, then measure it at the start view in gearboy:
`make clean && make PROFILE=1`, zero `prof_acc`..`prof_imgs` once in game,
and read `prof_acc[]` (scanlines per stage) once `prof_imgs` reaches 64.

When the working tree is ahead of HEAD (uncommitted build), take the baseline
from a copy of `src/` (`cp -r src tools/sim/baseX/src`) — `git archive` only
gives you the last commit.
