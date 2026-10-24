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
Slice 3: npc_test.txt (follower.state: walk east, look south/west, follower billboard), npc2_test.txt (littleroot.state, villagers),
  make_follower_state.txt (debug 'Cheat start' gives a party, toggling the view reloads the field so the follower spawns),
  follower_test.txt. harness command: pressflip KEYS MAX = frames from key press until the page flips (redraw latency).
Battle tests: battle_test.txt (wild encounter on Route 101, run away, view returns; needs route101.state from make_route_state.txt),
  trainer_test.txt + trainer_finish.txt (Route 102 scripted NPC text and trainer battle; needs route102.state from make_route102_state.txt;
  trainer_test.txt saves battle.state, trainer_finish.txt continues from it). Rerun all make_*state.txt after every rebuild.
  harness: walkloop N BLOCK (step until the view leaves Mode 4), watch ADDR|*PTRADDR MAXFRAMES (single-step until a byte changes, prints pc),
  dump ADDR LEN FILE.
Debugging the "stuck between two frames" report (hang in CB2_ReturnToFieldLocal / flicker loop):
  syms.txt: arm-none-eabi-nm -n ../../pokeemerald.elf > syms.txt (regenerate after every build; the harness reads static symbols from it)
  regs [TAG]        print DISPCNT, shadow GPU regs, camera, field callbacks, gMain callbacks, script state (run.sh resolves names)
  pc / strings / printers / watch / dump / framelog N   locate hangs (cpu pc + stack walk, assert text, text printer list, memory watch)
  fuzz SEED N       random button mashing (FUZZ_LOG=1 per-action log, FUZZ_NOIDLE=1 skips the idle tail); fuzz_one.sh + fuzz_report.py classify runs
  regress_stuck.sh  seeds that used to hang in ReturnToField (text printer node alive across the heap wipe)
  regress_loop.sh   seeds that used to loop view on / reload / view on (house, Mom's mat script after a warp with the view on)
  mom_test.txt      first-person in the house stepping onto Mom's mat; the script and its text run in the normal view
