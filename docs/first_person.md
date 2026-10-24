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
