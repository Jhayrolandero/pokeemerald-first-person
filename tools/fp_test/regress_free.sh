#!/bin/sh
# regress_free.sh: proves that the real game triggers fire from free movement (OW_FIRST_PERSON_FREE):
#   1. door warp:  walk north into Brendan's door in Littleroot  -> map 0/9 becomes 1/0 (the door warp check ran)
#   2. encounter:  walk into the Route 101 tall grass            -> a wild battle starts (per-step encounter check ran)
#   3. trainer:    walk into a Route 102 trainer's sight line     -> the trainer script runs and a trainer battle starts
# Needs littleroot.state, route101.state, route102.state (make_*state.txt) and syms.txt for the CURRENT build.
cd "$(dirname "$0")"
fail=0
out=$(./run.sh free_warp_test.txt)
if echo "$out" | grep -a "PLAYER" | tail -3 | grep -aq "map=1/0"; then echo "warp: ok (Littleroot 0/9 -> house 1/0)"; else echo "warp: FAIL"; fail=1; fi
out=$(./run.sh free_encounter_test.txt)
if echo "$out" | grep -a "GMAIN" | tail -1 | grep -aq "BattleMainCB2"; then echo "encounter: ok (wild battle started)"; else echo "encounter: FAIL"; fail=1; fi
out=$(./run.sh free_trainer_test.txt)
if echo "$out" | grep -a "GMAIN" | tail -1 | grep -aq "BattleMainCB2"; then echo "trainer: ok (trainer battle started)"; else echo "trainer: FAIL"; fail=1; fi
exit $fail
