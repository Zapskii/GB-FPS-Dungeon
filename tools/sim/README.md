# Test tools for render.s / copy_view (not part of the build)

Setup once: `python3 -m venv tools/sim/venv && tools/sim/venv/bin/pip install pyboy`.
Each tool builds a scratch copy of a `src/` dir with `bench.inc` (a bench
`main()`) in `tools/sim/bw/` through the gbdk-dev Docker image. `P=tools/sim/venv/bin/python`.

| Tool | Use |
|---|---|
| `bench.py SRC OUT.pkl [N]` | render N random views in PyBoy, dump outputs, rough T (timer) |
| `bench.py --cmp A.pkl B.pkl` | compare two dumps: ray arrays, seen[], OAM, and the two copied VRAM banks (build V no longer writes every vbuf row, so comparing against vbuf gives false differences) |
| `prof.py SRC [N] [ENTRY]` | exact cycles per label/instruction of an asm routine (SM83 interpreter `sm83.py`); `DEBUG=1 BYLINE=25` for C lines, ENTRY may be a hex address from a `-debug` .noi |
| `copysim.py SRC [N]` | copy_view cycle by cycle against a PPU model (`ppusim.py`, mode 3 172-289 T): VRAM writes in mode 3, store slack, late halts, lines per copy |

Baseline for A/B: `mkdir -p tools/sim/base && git archive HEAD src | tar -x -C tools/sim/base`,
then `$P tools/sim/bench.py tools/sim/base/src tools/sim/base.pkl 400`.
Run them from the repo root. PyBoy has no VRAM blocking: copy timing is only
trustworthy in copysim.py, mGBA or on a DMG (`make CHECK=1`).

Bench means are not the whole story: a change can win on 400 random views and
lose at the start view (the speed gate). Before integrating a render change,
check the per-view spread, then measure it in gearboy (PLAN.md procedure).

When the working tree is ahead of HEAD (uncommitted build), take the baseline
from a copy of `src/` (`cp -r src tools/sim/baseX/src`) — `git archive` only
gives you the last commit.
