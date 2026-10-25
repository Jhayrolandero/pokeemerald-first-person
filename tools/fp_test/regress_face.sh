#!/bin/sh
# regress_face.sh: item 4. On Route 102 the player turns away from a trainer (facing north), walks backwards into the trainer's sight
# line; when the trainer spots the player the player object (and the view) must turn to face the trainer (south).
# Needs route102.state + syms.txt for the CURRENT build.
cd "$(dirname "$0")"
out=$(./run.sh face_trainer_test.txt | grep -a "^PLAYER")
f_turned=$(echo "$out" | sed -n 2p | sed 's/.*facing=\([0-9]\).*/\1/')
f_sighted=$(echo "$out" | sed -n 3p | sed 's/.*facing=\([0-9]\).*/\1/')
if [ "$f_turned" = 2 ] && [ "$f_sighted" = 1 ]; then echo "face trainer: ok (player faced away = $f_turned, after the trainer spotted him = $f_sighted, towards the trainer)"; exit 0; fi
echo "face trainer: FAIL (turned=$f_turned sighted=$f_sighted)"; exit 1
