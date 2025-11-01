#!/bin/bash

set -e

curr_time=$(date +%Y%m%d%H%M%S)
input_dir_root=./gates-20251031050236-design-all-gates
rand_sample_count=2000

export FICTION_FOM_MAX_THREADS=8

for gate_dir in "$input_dir_root"/*; do
  [ -d "$gate_dir" ] || continue
  if ! compgen -G "$gate_dir"/*.sqd > /dev/null; then
    echo "Warning: no *.sqd files in $gate_dir" >&2
    continue
  fi
  stdbuf -oL -eL ./run_bpn_gate_fom.sh --input-dir "$gate_dir" --output-dir "$gate_dir" --sample-count $rand_sample_count --verbose | tee -a "logs-${curr_time}-run-fom-on-all-gates.txt"
done
