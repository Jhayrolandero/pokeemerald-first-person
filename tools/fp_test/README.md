fp_test: headless mGBA harness (libmgba from Homebrew) for the first-person view.
  ./build.sh                                   builds ./fp_harness
  ./fp_harness ../../pokeemerald.gba toggle_test.txt >/dev/null 2>&1   (run from this dir; stdout is emulator log noise)
  ./fp_harness ../../pokeemerald.gba restore_test.txt >/dev/null 2>&1  (L on, L off, compare rt_before/rt_after.ppm)
  ./fp_harness ../../pokeemerald.gba make_state.txt >/dev/null 2>&1    (re-creates littleroot.state: scripted new game to Littleroot)
  python3 ppm2png.py in.ppm out.png [scale]    convert shots; montage.py makes contact sheets
Script commands: run N | press KEYS N (A+B+L+R+UP... joined by +) | shot f.ppm | save f | load f | setvar 0xID val
                 | peek8 addr | waitmode MODE MAXFRAMES | waitflip MAXFRAMES (print frames waited, for timing)
IMPORTANT: savestates are tied to the exact ROM build (EWRAM/IWRAM layout). After every rebuild run make_state.txt again,
otherwise loading littleroot.state gives garbage (white screen, hangs).
setvar needs gSaveBlock1Ptr (read from pokeemerald.map) and the vars offset in harness.c (offsetof(SaveBlock1, vars)).
Slice 2 scripts: move_test.txt (fp_move_*), warp_test.txt (fp_warp_*), interact_test.txt (fp_talk_*), scripted_text_test.txt (fp_text_*, uses house.state).
Taps need >= 3 frames and ~30 frames spacing: input arriving while a redraw blocks the main loop is dropped.
