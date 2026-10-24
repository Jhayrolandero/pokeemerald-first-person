#!/bin/sh
# builds fp_harness against Homebrew libmgba
cd "$(dirname "$0")" && cc -O2 -I/opt/homebrew/include -L/opt/homebrew/lib harness.c -lmgba -o fp_harness
