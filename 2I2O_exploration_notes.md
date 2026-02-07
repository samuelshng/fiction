# Sam's notes on 2I2O exploration

## Build & test

Compilation:

```bash
cmake -B build -S . -DFICTION_TEST=ON -DFICTION_Z3=ON -DFICTION_ALGLIB=ON 
cmake --build build -j
```

Unit tests:

```bash
ctest --test-dir build --output-on-failure
```

## Run

A test script, [`./test_2i2o_flow.sh`](./test_2i2o_flow.sh), has been added for convenience, which outputs trial results in `./2i2o-exp-out`. When `--gold-grid` is not provided, `gold` runs on the Cartesian grid and `hex -io` is subsequently run; when `--gold-grid hex` is provided, `gold` directly places and routes on the hexagonal grid.

In the output directories, `*.fgl` files and relevant plots are included. (Plotting might become costly for larger network sizes, should add a sanity check before plotting/gate behind a flag.)

Some examples:

- Place and route `benchmarks/TOY/HA.v` with `all2` technology mapping, `gold` on Cartesian grid, and subsequent `hex`:
    ```bash
    ./test_2i2o_flow.sh \
        ./benchmarks/TOY/HA.v \
        --exp-name ha_all2_goldcart \
        --map all2 \
        --gold-effort-mode 3 \
        --gold-multithreading \
        --gold-seed 0 \
        --gold-tiles-to-skip-between-pis 0 \
        --gold-randomize-tiles-to-skip-between-pis \
        --gold-timeout 60 \
        --gold-cost-objective 0
    ```
- Place and route `benchmarks/TOY/RCA2.v` with restricted gate set (to encourage the use of HA tile) and `gold` on hexagonal grid:
    ```bash
    ./test_2i2o_flow.sh \
        ./benchmarks/TOY/RCA2.v \
        --exp-name rca2_ha_and_or_xor_inv_goldhex \
        --map ha,and,or,xor,inv \
        --gold-effort-mode 3 \
        --gold-multithreading \
        --gold-seed 0 \
        --gold-tiles-to-skip-between-pis 0 \
        --gold-randomize-tiles-to-skip-between-pis \
        --gold-timeout 60 \
        --gold-cost-objective 0 \
        --gold-grid hex
    ```

## Known limitations

- For the `RCA2.v` problem, when `all2` technology mapping is used the resulting network tends not to have HA tiles, thus the deliberate restriction on gate set
- When running in `--gold-grid hex` mode with `--gold-tiles-to-skip-between-pis` above 0, the run tends to fail. Haven't tested whether it's just a seed issue or some more deeply rooted issue.
