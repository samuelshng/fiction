#!/bin/bash

set -e

cmake -S . -B build -DFICTION_EXPERIMENTS=ON -DFICTION_ALGLIB:BOOL=ON
cmake --build build --parallel

# -a: find all gate candidates
# -s: save designs that were found
./build/experiments/practical_fanout_gate_designer "$@"
