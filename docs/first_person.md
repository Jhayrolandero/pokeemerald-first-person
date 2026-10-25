# First-person overworld view (prototype)

A togglable first-person, step-based "dungeon crawler" view of the overworld for pokeemerald-expansion.
It is a feasibility prototype: it works, but it is slow-ish and has rough edges (see Limitations).

## Enable

`include/config/overworld.h`:

```c
#define OW_FIRST_PERSON TRUE
```

In game, press **L** on the overworld to toggle. While on:

| Input | Action |
|---|---|
| Up | step forward (normal player movement, collision, warps, encounters, trainers) |
| Down | step backward without turning the view |
| Left / Right | turn the view 90 degrees |
| A | talk to / interact with what is ahead (leaves the view for the text, returns after) |
| L, Start, Select, R | leave the view (the press is swallowed) |

Followers (`OW_FOLLOWERS_ENABLED`) and NPCs are drawn as scaled billboards.

Scenery (smooth/free renderer, General tileset only): trees and the grey fence pieces are standing billboards made from their own metatile art,
with the grass background cut out (`MatteTex`, colour 0 = transparent), depth-tested against walls and sorted with the NPC billboards.
Beyond about 3 cells they fall back to the old wall blocks (cheap, and the trees are small there). Buildings stay wall blocks. Walking into a
dense treeline costs about 5 to 8 fps (30 -> 22-25), see `tools/fp_test/scenery_perf.txt`. Free movement speed is scaled by frames per tick when a
redraw takes more than 2 frames, so slow scenes do not slow walking down.

Walls whose distance is between FP_NEAR_LO and FP_NEAR_HI (about 1.6 to 2 cells, where a texel is 3 to 4 px wide) are drawn as two 2 px columns
instead of one 4 px column (shared depth, only the texture column differs), so facade detail such as window bars and signs stays sharp. Walls closer than that have
texels wider than the block already; farther walls stay cheap 4 px columns.

Fog and shading (smooth/free renderer): three fog bands (near under 2.5 cells = original palette, 2.5 to 4.5 cells a light tint, beyond that a strong haze
blend) from a (level, pixel) LUT, side faces darker and slightly cool, the last row pair of a wall within 4 cells is darker (own LUT level), a soft band on the floor
below walls within 4 cells and a dark 2 px line on the side face at building corners (post pass `WallShading`, slot -> darker slot maps). New shading stops adding palette
colours above 232 slots (`FP_DARK_MAX`) instead of falling back to nearest-colour errors. `gFpPolishOff` (debug) shows the previous look.

## How it works

Code: `src/first_person.c`, `include/first_person.h`, hooks in `src/overworld.c`, `src/field_control_avatar.c`, `src/malloc.c`.

- Takes over the display in BG Mode 4 (240x160, paletted, double buffered), 120 columns drawn 2 px wide.
- Textured raycaster over the real map: wall facades and floor sample the actual metatile pixels from the tilesets.
  A per-redraw 255-colour palette is built on the fly, with distance fog and the time-of-day tint.
- NPC, trainer and follower object events are scaled in software and depth-tested against wall distance.
- Redraws only on toggle, step, turn or when a visible object changes (5 to 13 frames per redraw).
- Cost: about 46 KB of heap while active, all of VRAM is overwritten (leaving reloads the field like a menu return), about 300 bytes of IWRAM for the hot loop.

## Limitations

- Each redraw is a visible pause: this is a stepping view, not smooth movement.
- Menus and text boxes cannot show in Mode 4. Text that scripts start by themselves (trainer sight, step triggers) is lost; the view drops back to normal.
- Billboards do not rotate with facing, have no shadows; water does not animate; side/back building faces reuse the front art.
- Tree vs building is told apart by a metatile id heuristic.
- IWRAM is nearly full (about 2.9 KB spare); adding IWRAM code can crash the game at boot.
- A wild encounter (run away) and a trainer battle (win) were tested in first-person and return to the view; other heap-heavy flows are untested. NPCs and followers are skipped if under ~14 KB of heap is free.

## Test harness

`tools/fp_test/` has a headless libmgba harness (requires `brew install mgba`) and screenshots. See `tools/fp_test/README.md`.

## Handoff notes (for the next developer or agent)

- Branch history: `first-person` (stepping view) -> `fp-smooth` (gliding camera) -> `fp-free` (free movement, floor, sky, scenery, fog). `fp-free` is the latest.
- Modes (Select cycles): free, smooth, stepping. L turns the view on, Start turns it off. Flags: `OW_FIRST_PERSON`, `OW_FIRST_PERSON_SMOOTH`, `OW_FIRST_PERSON_FREE`.
- Budgets (emulator numbers only; never verified on real hardware): walking about 25 fps, IWRAM nearly full (the stack overflows past about 29,900 B of IWRAM use, so keep new code/data in ROM or EWRAM), 255 shared Mode 4 palette colours, heap about 60 KB while active.
- Savestates and `tools/fp_test/syms.txt` are tied to the exact build: run `tools/fp_test/rebuild.sh` after every rebuild.
- Regression suite: `tools/fp_test/regress_*.sh` (seven scripts; the follower-after-battle one is timing-sensitive, rerun once before calling it a failure).
- Test hooks: `gFpBillLog` (guarded by `FP_TEST_HOOKS`), `gFpNearOff`, `gFpPolishOff` (redraw the previous look for identical-pose before/after shots).
- Known gaps: NPC contact shadows and feet alignment not done; bike/surf/rock smash in free mode not reviewed; scenery billboards only for the General tileset; text started by scripts is shown in the normal view after a reload.
