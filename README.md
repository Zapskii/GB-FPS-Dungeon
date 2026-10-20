# GB-FPS-Dungeon — DMG raycaster crawler

A first-person Wolfenstein-style raycaster for the original Game Boy (DMG),
built in GBDK-2020 with hand-written assembly for the renderer. Four
randomly generated corridor-maze floors (one-way stairs down), keys and
doors, two pathfinding billboard enemy types (grunts and bats), health,
gold and map-scroll pickups that scale with distance, shooting with an
on-screen gun that lowers over items, an automap, sound effects, title /
game-over / win screens, a riveted frame around the view, HUD compass,
hearts and gold. The
3D view is a real 128×64 bitmap (64 rays, 2-px columns) drawn into tile
data and double-buffered across the two BG tile banks; HUD in the Window.

Build marker: the character in the middle of the HUD's bottom row (now
**T**; 1–9 then letters) identifies the current build; it changes between
hand-offs.

## Build

    make            # docker gbdk-dev -> dungeon.gb (32 KB)
    make clean

No local GBDK needed: the Makefile runs `/opt/gbdk/bin/lcc` inside the
`gbdk-dev` image.

## Controls

| Input | Action |
|---|---|
| D-pad up/down | walk forward/back |
| D-pad left/right | turn |
| B + left/right | strafe |
| A | shoot (hitscan at screen centre, ~4 shots/s) |
| Select | automap (pauses; B toggles music, shown under the map; Select/A/Start closes) |

Items are white and scale with distance (about 4, 8 and 16 px). Walk
over a key to take it, touch a **D** door (dark grey) with a key to open it,
walk to the down-arrow (stairs) to descend. Hearts restore one heart (left
lying if you're at full health), gems add to the gold count (HUD, next to
the floor number), and a scroll reveals the whole floor on the automap.
Grunts (dark blobs) walk the floor; bats (light wings, flying at eye
level) move twice as fast and die in one shot; brutes (a grunt drawn one
size bigger, floors 3–4) move at half speed and take three shots. Floor n
has n bats, 1 brute on floor 3 and 2 on floor 4, and grunts up to 8
enemies (5, 5, 4, 2). Enemies hurt on contact
(screen flashes inverted); one shot kills grunts on floors 1–2, two on
floors 3–4 (a hit enemy flashes white and is stunned briefly); at 0 hearts
the run ends on a GAME OVER screen (floor reached, kills, gold). Clear
floor 4 to escape: a win screen shows kills, seconds played (map time
excluded) and gold.
Start on either screen begins a fresh run; a title screen opens the game.
HUD row 1, right: frames per second (views flipped per 60 VBlanks,
counted and written by the VBL ISR every second).

## Architecture

### Display path (bitmap in tile data, bank flip via LCDC.4)

The viewport is 16×8 BG tiles (indices 0–127, column-major) centred on a
black border. With LCDC.4 = 1 those indices read 0x8000–0x87FF (bank A),
with 0 they read 0x9000–0x97FF (bank B); indices 128+ are shared by both
modes, so the border, HUD (base 160) and sprites live there.

Per image the main loop: runs game logic once per elapsed VBlank (60 Hz
regardless of render rate, capped at 4 catch-up frames), renders into
`vbuf` (WRAM, 2 KB, laid out as the 128 tiles), waits for any pending flip,
`copy_vbuf()` streams it into the hidden bank, updates shadow OAM and sets
`flip_req`. The VBL ISR toggles LCDC.4 in the same VBlank as the OAM push
and also runs `hud_update()`, so the main loop never idles for VBlank. The
ISR must stay well inside VBlank (copy_vbuf's safety relies on it).

`copy_vbuf` polls STAT before every 2-byte burst. Once mode 0/1 is seen
VRAM stays open ≥ 80 T (rest of mode 0 + all of mode 2), and both stores
land within ~52 T, so it is safe whatever sprites do to mode 3 — unlike the
old interrupt-driven hblank streaming, whose 200–400 T handlers spilled
into mode 3. Verified on a physical DMG (2026-10-19). mGBA enforces
mode-3 blocking; gearboy does not.

### Renderer (`src/render.s`, assembly)

- 64 columns from the camera plane (`dir + plane·x`, set up per image by
  `setup_rays()` in C), so the DDA distance is already perpendicular: no
  fisheye correction. Amanatides & Woo DDA in 8.8; side distances in BC/DE.
- Adaptive casting: even columns (and column 63) are always cast; an odd
  column whose neighbours hit the same map cell from the same side copies
  their cell/type and averages their top/bot/zb (H is linear in screen x
  across a flat face, so this is exact up to rounding), else it is cast.
  Per column: `r_top`, `r_bot`, `r_zb`, `r_hits`, `r_wt` (type | side << 7);
  a separate colour pass then builds `r_clr`.
- Ray components are int8 at |dir| = 88.3 so the FOV corners fit; `rcp[]`
  is the matching one-cell step. Height = `ht[perp >> 3]` (2048/n, 2–64 px).
- Shading: x-facing walls colour 0, y-facing 1, doors 2. The first ray on a
  new map cell/face is one shade darker, which outlines blocks and corners
  (and darkens far walls, where every ray is a new cell).
- Fog: plain walls 3–6 cells away get the right pixel of each 2-px ray one
  shade darker (vertical-stripe dither, constant per row so the core fill
  stays one byte pair); beyond 6 cells both pixels. Edges and doors are
  never fogged. `r_clr` holds two colours per ray (bits 0–1 left, 2–3 right).
- Ceiling and floor come from a 64-row pattern table (`bg_tab`, generated by
  `tools/gen_bgtab.py`): ceiling dark grey near the viewer fading through
  line dithers to black at the horizon; floor black at the horizon through
  dots and checkers to light near the viewer. ~+4% render cost.
- Fill per tile column (4 rays): ceiling to the highest top, a constant
  all-wall core, floor from the lowest bottom, then read-modify-write only
  the ragged rows of each ray.
- Multiplication uses log/antilog tables (~230 T; ~1.5% error).
- `r_zb[64]` keeps the perpendicular distance per ray for sprite occlusion
  and fog.
- `=` walls: after the fill, three dark mortar rows at 1/4, 1/2 and 3/4 of
  the projected wall height (walls ≥ 8 px), so they read as brick and
  converge with perspective; `#` walls stay plain.
- `src/textures.h` is no longer used (it was the old tile-strip texture set).

### Enemies and items (`src/ai.h` movement, `src/sprites.h` art)

8 entities moved on every 4th VBlank by `src/ai.h`: a BFS distance field
from the player's cell (rebuilt only when that cell changes or a door
opens, capped at 10 steps / 128 cells, ~1.3 KB RAM); each enemy walks
downhill along corridors, centring on the cross axis before advancing so
it never cuts corners (6 units a step, `ai_sp` = 12 for bats, 3 for
brutes). Farther than 10 steps they idle. `etype[]`: 0 grunt, 1 bat,
2 brute (`enemy_art` row 2 = the grunt tiers shifted one up, no new tiles).
Hit points `ehp[]` (1, 2 for grunts from floor 3, 3 for brutes); a non-lethal hit sets
`ehit[]` = 3 AI ticks of stun, drawn with OBP1 so the sprite flashes.
Contact damage under 0.78 cell with a 60-tick cooldown, driving `hp`
and the HUD hearts. `project()` is shared by billboards, item sprites and
`shoot()`; flashes are a BGP override counted down in the VBL handler.
Sprites are billboards in three distance tiers. Grunts: 8×16 far, 16×16
mid, 24×32 near (at 3 and 1.5 cells). Bats: ~4, 8 and 16 px wide, bottom at
screen y 68 with a ±2 px bob. Items: tiny, normal, 16 px (at 4 and 2
cells). `tools/gen_sprites.py` turns ASCII art into `src/sprites.h`
(29 OBJ pairs at 156–159 and 184–237); item big tiers are the normal
art doubled, tiny ones hand drawn or halved, and a pair that mirrors an
earlier one is stored once and drawn with X-flip (bit 0 of the tile byte
in the art lists). `draw_art()` places any `{w, h, tiles}` art by its
bottom centre; `shoot()` uses the art width as the hit half-width.
Projection is int32 forward/lateral (int16
overflows at 768×127), screen x via a shift-subtract divide, culled by
range and FOV, hidden when the wall at their column is nearer, feet placed
on the floor line. Up to 3 enemies are drawn nearest first, and every
billboard after the gun must fit a width budget of 7 sprites (DMG shows
the first 10 OBJ per line in OAM order; the gun takes 3), so a far one is
skipped whole instead of a near one losing columns. While an item is
drawn behind the gun, the gun slides down 1 px per tick to 16 px (back up
2 per tick): `gun_shift()` rewrites the idle art shifted inside its own 6
tiles (STAT-safe copies, 192 bytes per change), so rows past the bottom
drop off instead of spilling onto the frame. No recoil frame while
lowered (the screen still flashes). Computed after the copy from the
same camera the view was rendered with, and pushed in the flip VBlank.

### Levels (`src/mazegen.h`, generated on the cartridge)

Every floor is generated when it is entered, so each run gets new mazes.
The RNG is seeded from DIV and the frame counter when Start is pressed on
the title screen. `maze_generate()` (a C port of the old Python tool) builds
a 1-wide corridor maze (recursive backtracker on odd cells), carves a few
small rooms, knocks some dead ends through into loops (fewer rooms/loops on
later floors), then BFS from the start: key in the farthest dead end,
stairs in another far dead end whose only neighbour is a plain corridor
cell, which becomes the door (so the door seals exactly the stairs alcove
and no flood-fill validation is needed), grunts and bats ≥ 6 steps away,
2 health, 2 gold ≥ 4 steps, a map scroll ≥ 8, 15% of walls as `=`. It writes the char map
into `vbuf` and uses `seen[]` and the back of `vbuf` as scratch (both are
rewritten afterwards), so it costs no extra RAM.
Map grammar: `#` wall, `=` wall 2, `D` door, `K` key, `S` stairs, `H`
health, `G` gold, `M` map scroll, `P` start, `1`–`8` grunts, `B` bats,
`X` brutes. `load_level()` parses it into the
collision map, items, spawns, and camera; keys reset per floor, HP carries,
the floor-4 stairs show `win_screen()`, running out of hearts shows
`game_over()`; both start a fresh run on new mazes.

### Automap (Select)

`seen[1024]` marks cells the player has stood next to (3×3 on entering a
cell) plus the wall cell every ray hit (`r_hits[64]`, stored by render.s).
`show_map()` pauses the game, draws explored cells at 2×2 px into `vbuf`
(64×64 in the middle of the viewport: floor dark, wall light, door/key/
stairs white, unexplored black), copies and flips like a normal frame, and
shows the player as a white arrow sprite: the one direction needed is
copied from `arrow_tiles` (8 directions) into OBJ pair 154–155 each time.
Cleared on level load.

### Sound (`src/sfx.h`)

Fire-and-forget APU effects, one const table row each (NR10–14 for the
ch1 square/sweep, NR41–44 for ch4 noise); hardware length/envelope ends
them, no per-frame driver. SHOOT, HURT, KEY (also health), DOOR, STAIRS,
KILL, HIT. Effects
on the same channel cut each other off; ch2/ch3 are free. Note: gearboy's
IO memory view shows stale NR51/NR52 values, so check audio by ear.

### Frame and gun

`src/frame.h`: a 1-tile riveted metal bezel around the viewport on a dark
brick backdrop (BG tiles 244–254), drawn by `frame_draw()` from
`draw_frame()`. `src/gun.h`: two-handed pistol, 3×2 sprites of 8×16 in OAM
slots 0–5 (tiles 130–141 idle, 142–153 recoil with a white flash via OBP1),
bottom edge on the viewport floor line; `gun_bob` advances while moving for
a ±2 px sway, recoil shows while `fire_cd > 9`. Billboards start at slot 6. Over an item the gun slides
down inside its own tiles (`gun_shift()`, see Enemies and items).
Art sources: `tools/gen_gun.py` (ASCII art → `src/gun.h` + preview PNG),
`tools/gen_frame_tiles.py` and `tools/preview_frame.py` for the frame.

### Title / game over / win screens

`text_screen()` (display off, BG map rewritten as centred white banners,
waits for Start) backs the title, `game_over()` and `win_screen()`. Text
uses the HUD font plus 13 extra letters at base 184 (`font_tiles`: YUCAPD
TILR GMV; M is 5 px wide so it doesn't read as N; O reuses digit 0).
Those tiles hold sprites during play, so the font is loaded for each text
screen and `draw_frame()` + `sprites_init()` restore frame and sprites
afterwards. The build marker glyph is copied into BG tile 255.

### HUD

Arriving on a floor shows a white " FLOOR n " strip across view tile
row 1 for 1.5 s (`banner_t`). The VBL ISR writes it into the BG map and
afterwards puts the view tile indices back; `text_screen()` cancels it.
L and R are copied from the font into tiles 238–239 at boot.

Window row 0: 5 hearts, `K n`, `F n`, gem + 2-digit gold (gem = BG tile
183), compass `N E S W` with the facing cardinal inverted. Row 1: the
build marker and the FPS counter. Tiles at base 160 (`src/hud.h`, 23
tiles).

## Milestones (measured with the gearboy profiler/MCP)

| Milestone | Result |
|---|---|
| M1 streaming proof | 288 IRQs/window traced, capacity 576 vs 320 demand |
| M2b asm renderer | 421k T (C) → 96k T (asm) |
| M2c-1 perf | 59.5k T, 2-frame builds (30 fps greybox) |
| M2c-2 textures | 74.9k T, ~21 fps content |
| M3a HUD | compass/hearts/keys/floor |
| M3b enemies | billboards, chase AI, contact damage |
| M3c levels | 4 validated maps, keys/doors/stairs, death restart |
| Final display | VBlank-only, worst frame ~4.2k/4,560 T, ~15 fps content |
| Pixel renderer | 128×64 bitmap, 64 rays, STAT-polled copy, LCDC.4 flip; works on hardware |
| Gameplay 1 | shooting, key/stairs sprites, hurt/shot flashes (build 1) |
| Gameplay 2 | corridor-maze generator, start facing an open corridor (build 2) |
| Automap | Select map of explored cells + ray-seen walls (build 3) |
| Win screen | floor 4 → stats screen → new run (build 4) |
| Sound + AI | APU effects, BFS pathfinding enemies (build 5) |
| Display polish | frame, gun sprite, ceiling/floor gradients, wall fog (build 6) |
| Screens + FPS | title/game-over screens, FPS counter, VBL-ISR flip (builds 7–A) |
| Random mazes | on-cart generator per run, health pickups, 2-hit enemies (build C) |
| Speed | adaptive rays, log side distances, PLAN.md waves 1–4: 6.5 → ~10.75 fps (builds D–I) |
| Variety | bats, gold and map items, item size tiers, sprite generator (build L) |
| Sprite polish | gun lowers (M) then slides (P) over items; nearest-first, line-budgeted billboards (M) |
| Floor banner | "FLOOR n" across the view for 1.5 s on arrival (build R) |
| Brute | third enemy type: grunt art one size up, half speed, 3 HP, floors 3–4 (build T) |

## Constraints & next steps

- Frame rate (build 8, gearboy, long corridor view): ~7 fps ≈ 8.6 frames
  per image. Profile: 64 × `cast_ray` ≈ 3.3k T each ≈ 3 frames (DDA steps
  ~1.8k, side-distance multiplies ~1.5k); copy ≈ 2 frames (near its STAT-
  polled limit, VBlank-only bursts would gain ~3%); fill + C ≈ 1.5 frames.
  Adaptive casting (build D), A/B in gearboy with a fixed seed and the
  same start view, images per 4 s: all 64 rays 26 (6.5 fps), adaptive 29
  (7.25 fps), +12%. A profiler sample showed ~1/3 of columns interpolated
  (34 casts vs 16 averaged). Build E: side distances as one log-domain
  lookup (`sd_lu`, log8 of the camera fraction once per image, `lrc[]`
  table; `tools/check_sd.py` checks it, worst error 3.7%) and one DDA
  loop per ray quadrant with no sign tests or step counter (~105–130 T
  per step, was ~250). Same A/B: 29 → 33 images per 4 s (7.25 → 8.25
  fps, +14%), image unchanged.
- Stage profile (build E, `make clean && make PROFILE=1`, fixed seed,
  start view, 64 images; zero `_prof_acc`..`_prof_imgs` with gearboy
  write_memory once in game, then read `prof_acc[]` = scanlines/stage):
  copy 2.28 frames, rays + colour + mortar 2.10, fill 1.87, sprite_update
  0.49, logic 0.32, seen[] 0.15, rest 0.14; total 7.34. See PLAN.md.
- Build F (PLAN.md wave 1): generated ceiling/floor writers (fill -9.8k T
  measured), asm udiv8/smul8 + foot_tab in project() (~5.5k T per visible
  object), BFS rebuild in asm without the 1 KB memset (open-room worst
  case ~271k -> ~76k T, simulated). Start view still 33 images / 4 s
  (~0.2 frames saved, under one image); the BFS gain shows as less
  stutter while enemies re-path, not at the static start view.
- Build G (PLAN.md wave 2): `copy_view` copies per tile column only rows
  [min top, max bot) of this image's rays united with the span last
  written to the same VRAM bank (`r_vh`); `view_reset()` after level load
  and the Select map. Profiler at the start view: copy 2.28 -> 2.04
  frames (-16.9k T; near side walls fill most rows here, so the gain
  grows in open views). Total 7.39 (build E) -> 6.96 frames per image.
  Checked in gearboy: no stale strips turning, at a wall, after the map.
- Build H (PLAN.md wave 3): unrolled ragged-edge patch (RTOP/RBOT
  macros), colour pass folded into ray_cs + a short fog loop, seen[]
  marked in dda_hx/dda_hy instead of the C loop. Byte-identical to G on
  3,300 random views in an SM83 interpreter. Profiler at the start view:
  render_asm -39.5k T, seen[] 0.15 -> 0.02; total 6.26 frames per image
  (~9.6 fps, HUD shows 09-10). Remaining per-label hot spots: wall-band
  loop f_cl ~28k T, mortar ~30k T, rd_step2 ~15k T.
- Build I (wave 4): wall band as an unrolled `.rept` block (fc_blk, 24 T
  per row), mortar only for '=' walls with one row address per column
  (mo_col), ray directions from per-image tables (r_dxs/r_dys), camera
  cell once per image, sd_lu/od_avg inlined. Byte-identical to H on 5,300
  random views. Profiler: render_asm 3.31 -> 2.63 frames (-47.8k T);
  total 5.58 frames per image (~10.75 fps, HUD 10-11). Build H's HUD
  marker shows blank: the font has no H (or B) - next markers U, V, Y
  (I, L, M, P, R, T, U used).
- ROM: ~22 KB of 32 KB used (no MBC). Saves or more floors need an
  MBC5 switch (`-Wl-yt0x1B`). WRAM: ~6 KB of 8 KB used.
- Build I tested OK in mGBA (2026-10-20, enforces mode-3 VRAM blocking).
  Still worth watching: an AI rebuild in an open room can take ~1 frame.
- Build L (enemy types, items, item size tiers) tested OK in mGBA and on
  a physical DMG (2026-10-20). Build M (lowered gun, sprite line budget):
  gun drop confirmed by the user. Build P (gun slides instead of jumping):
  confirmed working by the user (2026-10-20). Build R (floor banner):
  checked in gearboy. Build T (brute): checked in gearboy. Build U
  (music, B toggles it on the map) works on a physical DMG (2026-10-20).
- Free shared tiles: 240–243 (2 OBJ pairs or 4 BG tiles); 238–239 hold
  the banner's L and R.
- Decided: stairs stay one-way (no going back up a floor).
- Ideas not done: full wall textures.
  `textures.h` is dead.
- Test display-path changes in **mGBA or hardware** — gearboy does not
  enforce mode-3 VRAM blocking.

## Gotchas (hard-won)

- sdasgb has no `ld hl,(nn)` / `ld (nn),hl` and no `neg` (use `cpl/inc`).
  Use HRAM byte pairs via `ldh` for 16-bit pointers in ISRs.
- `__at(addr)` HRAM variables get mangled into undefined hash symbols by
  SDCC; use pointer-cast macros like GBDK's register map.
- SDCC: `uint8_t` loop counters against 1024 never terminate (warning 94);
  statics must be declared before use; ternaries and combined conditions
  can trigger "conditional flow changed by optimizer" miscompiles — use
  plain nested if/else in hot C.
- SDCC "warning 158: overflow in implicit constant conversion" on the
  build-marker line is spurious (the value is 160+n < 256).
- gearboy MCP testing: symbol addresses are in `dungeon.noi` (globals
  only; statics such as `ex`/`ey`/`ealive` sit just after `cam_x`/`cam_a`).
  Memory reads can leave the emulator paused (call continue), screenshots
  lag a few frames, IO reads of NR5x are stale, and enemy contact can kill
  the player and reload the level mid-test (resetting the camera).
- Instruction cost discipline: T-cycles are 4× M-cycles; `ld (de),a` +
  `inc de` is 16 T, so a 320-byte copy is ~7,700 T, not ~2,000.
