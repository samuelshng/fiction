# 2026-03-11 Hex GOLD Scalability Exploration

## Scope

This note records the native-hex `gold` scalability investigation performed on 2026-03-11, focused on the `w4a4` benchmark and cross-checking against `w2a2` so that changes would not accidentally break the smaller case.

The target question was:

- Why does native-hex `gold` still fail on `w4a4` while older non-2i2o cartesian `gold` had partial success?
- Can native-hex `gold` be pushed far enough to actually place and route `w4a4` without regressing `w2a2`?

This note is intentionally detailed and incremental. The goal is that another developer or agent can read it and know:

- what was tried,
- what each step changed,
- what worked,
- what did not work,
- what was reverted conceptually,
- and what is still worth trying next.

## Baseline Context

### Older non-2i2o baseline

From `/home/samuelshng/git/verilog-mxu-bak-after-tnano-paper/gold_sweep_results`:

- `w4a4 g5 cost1`: `9 / 105` successes
- `w4a4 g6 cost1`: `48 / 105` successes
- `w4a4 g6 cost4`: `49 / 105` successes

These were older cartesian results from before the 2i2o work, so they are not apples-to-apples with current native-hex runs, but they are still useful as a reminder that `w4a4` is not inherently impossible for the broader GOLD family.

### Current mapped network size

Using current `fiction` with `read -a ...; map --all2; ps -n` on `w4a4`:

- network: `w4a4_deepsyn3652 (TEC)`
- I/O: `37 / 37`
- mapped gates: `196`
- levels: `25`

Gate breakdown from `gates -n`:

- `AND2 = 38`
- `OR2 = 18`
- `NAND2 = 71`
- `NOR2 = 13`
- `XOR2 = 21`
- `XNOR2 = 6`
- `INV = 12`
- `BUF = 17`
- `total = 196`

This matters because the large-instance heuristics used below were keyed off `num_pis >= 32` and `num_gates >= 150`.

### Native-hex sweep state before this exploration

From `/home/samuelshng/git/verilog-mxu/gold_sweep_results_2i2o_20260310_test_03_hex`:

- all `w4a4 g5/g6 cost0/cost1` runs were `0 / 106`
- failures were true native-hex GOLD failures, not downstream crashes
- best old `w4a4 g6 cost0` seed depths from the artifact logs were:
  - seed `66`: `264`
  - seed `93`: `258`
  - seed `86`: `254`
  - seed `81`: `247`

So before any changes on this day, native hex was not finishing `w4a4`, but some seeds were materially deeper than others.

## Files Touched During This Exploration

- [`include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp`](/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp)
- [`include/fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp`](/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp)
- [`test/algorithms/physical_design/graph_oriented_layout_design_hex.cpp`](/home/samuelshng/git/fiction-sam/test/algorithms/physical_design/graph_oriented_layout_design_hex.cpp)

## Experiment Sequence

### 1. Add per-SSG depth tracking

Change:

- Added `max_placed_nodes` to `search_space_graph` in [`graph_oriented_layout_design.hpp`](/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp).

Reason:

- Needed to see not only the global best depth but also how each search-space graph was doing, so that large-instance pruning decisions could be made on actual depth information instead of only frontier size or queue size.

Outcome:

- Useful and kept.
- This remains a good diagnostic aid regardless of future heuristic changes.

### 2. First attempt: short pilot on 4 SSGs, then prune to the “best” one

Initial idea:

- For large native-hex instances, 16 or 32 SSGs looked too shallow.
- The first attempt was to:
  - start with `4` SSGs instead of `32`,
  - give them a short pilot window,
  - score them by frontier/depth/queue state,
  - then prune to a single survivor.

Code involved:

- [`graph_oriented_layout_design_hex.hpp`](/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/graph_oriented_layout_design_hex.hpp)

What was changed:

- Added `should_prioritize_search_depth()`
- Added `should_use_search_depth_pilot()`
- Added `calculate_search_depth_pilot_timeout()`
- Reduced large-instance `num_search_space_graphs` from the usual maximum-effort budget to `4`
- Added `prune_to_best_search_space_graph()`
- Added a regression in [`graph_oriented_layout_design_hex.cpp`](/home/samuelshng/git/fiction-sam/test/algorithms/physical_design/graph_oriented_layout_design_hex.cpp) to check that large wide native-hex instances use the reduced batch

Immediate problem:

- This version introduced a real crash.
- Targeted CLI reruns on `w4a4` exited with `rc=139` before any JSON log file was written.

Root cause:

- The large-instance path reduced `ssg_vec` to `4`, but the later highest-effort/maximum-effort initialization code still wrote into offsets assuming the full 16/32-SSG layout.
- This caused out-of-bounds writes in SSG initialization.

What was done next:

- Fixed the out-of-bounds initializer path so that large-instance reduction does not try to populate missing SSG slots.

Status:

- The crash itself is fixed.
- The pilot strategy was later abandoned for performance reasons, but the correctness fix from this phase was important.

### 3. Fix the pilot-prune crash mechanics

After the first crash fix, another issue remained in the pilot path:

- `prune_to_best_search_space_graph()` originally shrank `ssg_vec` destructively.
- That looked risky in the presence of ongoing loop logic and frontier reuse.

Adjustment:

- Changed pruning so that non-winning SSGs are deactivated and cleared instead of physically removing all but one vector element.

Outcome:

- This made the pilot path stable enough to benchmark.
- However, it turned out not to be the right search strategy for `w4a4`.

### 4. Benchmark the repaired pilot-prune strategy

Targeted reruns on the current tree:

- `w4a4`, seed `66`, `g=6`, `-j`, `-t 30`, `cost=0`, `--grid hex`
  - `max placed nodes = 94`
  - `num search space graphs = 4`
- `w4a4`, seed `66`, same settings plus `--prefer_input_pin_order --prefer_output_pin_order`
  - `max placed nodes = 16`
  - `num search space graphs = 4`
- `w4a4`, seed `66`, `g=6`, `-j`, `-t 120`, `cost=0`, `--grid hex`
  - `max placed nodes = 196`
  - `num search space graphs = 4`
- `w4a4`, seed `81`, `g=6`, `-j`, `-t 120`, `cost=0`, `--grid hex`
  - `max placed nodes = 210`
  - `num search space graphs = 4`
- `w4a4`, seed `81`, `g=6`, `-j`, `-t 400`, `cost=0`, `--grid hex`
  - `max placed nodes = 210`
  - `num search space graphs = 4`

Interpretation:

- The pilot path was no longer crashing.
- But it plateaued badly.
- The strongest seed at `120s` and `400s` reached the same depth (`210`), which strongly suggests the pilot was choosing a branch that looked good early but then got stuck.

Conclusion:

- The pilot-and-prune strategy was not just insufficient, it was actively worse than the simpler “one deep search” strategy that had been tried earlier.

What was kept:

- The crash fix and the large-instance regression coverage.

What was discarded conceptually:

- The pilot-prune strategy as the preferred large-instance search policy.

### 5. Return to the simpler one-SSG deep-search strategy

Change:

- Disabled the pilot path in practice and reverted large-instance native-hex maximum-effort to use a single deep SSG.

Code state:

- Large wide instances now collapse directly to `1` SSG.
- Regression test was updated accordingly:
  - [`graph_oriented_layout_design_hex.cpp`](/home/samuelshng/git/fiction-sam/test/algorithms/physical_design/graph_oriented_layout_design_hex.cpp)
  - test name now reflects a single deep search-space graph

Measured results:

- `w4a4`, seed `81`, `g=6`, `-j`, `-t 30`, `cost=0`, `--grid hex`
  - `max placed nodes = 105`
  - `num search space graphs = 1`
- `w4a4`, seed `81`, `g=6`, `-j`, `-t 120`, `cost=0`, `--grid hex`
  - `max placed nodes = 230`
  - `num search space graphs = 1`
- `w4a4`, seed `81`, `g=6`, `-j`, `-t 400`, `cost=0`, `--grid hex`
  - `max placed nodes = 338`
  - `num search space graphs = 1`

This is materially better than the repaired pilot path:

- pilot at `120s`: `210`
- one deep search at `120s`: `230`
- pilot at `400s`: `210`
- one deep search at `400s`: `338`

Outcome:

- This is the best measured native-hex `w4a4` behavior reached during this exploration.
- It still does not complete, but it is meaningfully better than both the old all-fail sweep behavior and the 4-SSG pilot strategy.

Status:

- Kept.

### 6. Check whether the `w2a2` side would be harmed

Cross-check run:

- `w2a2`, seed `13`, `g=2`, `-j`, `-t 30`, `cost=0`, `--grid hex`
  - `max placed nodes = 54`
  - `num search space graphs = 32`

Interpretation:

- `w2a2` does not trigger the large-instance threshold.
- So the large-instance one-SSG fallback does not change `w2a2` behavior.
- This was the intended safeguard and it held.

### 7. Re-test soft pin-order preference on `w4a4`

This was important because earlier work on `w2a2` had shown benefit from:

- `--prefer_input_pin_order --prefer_output_pin_order`

But on `w4a4`, every tested configuration showed the opposite.

Measured examples:

- 4-SSG pilot mode, seed `66`, `t=30`
  - without preference: `94`
  - with preference: `16`
- 1-SSG deep mode, seed `81`, `t=30`
  - without preference: `105`
  - with preference: `10`

Conclusion:

- Soft pin-order preference helps some `w2a2` native-hex runs.
- It is strongly harmful on `w4a4` under the tested settings.
- It should not be made a default for large native-hex runs.

### 8. Try explicit pin orders derived from a successful cartesian `w4a4` layout

Idea:

- Maybe the problem is not “pin preference is bad” but rather that the declaration order is bad.
- So extract PI/PO order from a successful cartesian result and feed that to native hex as a soft preference.

Source used:

- `/home/samuelshng/git/verilog-mxu/gold_sweep_results_2i2o_20260310_test_01/artifacts/gold_sweep_results_w4a4_g6_cost0/seed_00057/gold.fgl`

Experiment:

- Extracted PI order and PO order from the cartesian layout
- Ran native hex with:
  - `seed=57`
  - `g=6`
  - `-j`
  - `-t 30`
  - explicit `--input_pin_order ...`
  - explicit `--output_pin_order ...`

Result:

- `max placed nodes = 105`
- `num search space graphs = 1`
- no layout

Interpretation:

- Cart-derived explicit interface order is not catastrophically bad like declaration-order preference, but it also did not unlock any new behavior.
- It performed roughly like an ordinary decent 1-SSG run, not like a breakthrough.

Conclusion:

- Explicit order derived from cartesian `w4a4` is not the missing ingredient.

### 9. Try more PI spacing and more candidate expansions

These were low-risk knobs worth checking once the search was stable.

#### Increase `g`

Test:

- `w4a4`, seed `66`, `g=7`, `-j`, `-t 30`, `cost=0`, `--grid hex`

Result:

- `max placed nodes = 77`

Comparison:

- same run at `g=6`: `94`

Conclusion:

- More PI gap did not help here.
- `g=7` was worse than `g=6` on the tested seed.

#### Increase `num_vertex_expansions`

Tests:

- `-n 6`
- `-n 8`

Results for seed `66`, `g=6`, `t=30`:

- `-n 6`: `94`
- `-n 8`: `93`

Baseline:

- default: `94`

Conclusion:

- Increasing `num_vertex_expansions` did not materially help.
- This is not a case where the current search is simply starved by too few per-vertex candidate expansions.

### 10. Check randomness from `-j`

Test:

- 1-SSG deep mode, seed `81`, `g=6`, `t=30`, cost `0`, no `-j`

Result:

- `max placed nodes = 99`

Comparison:

- same setup with `-j`: `105`

Conclusion:

- Randomized PI skip is mildly helpful on this seed, not harmful.
- It is not the main blocker, but removing `-j` also does not rescue `w4a4`.

### 11. Check whether cost objective matters

Test:

- 1-SSG deep mode, seed `81`, `g=6`, `-j`, `t=30`, `cost=1`

Result:

- `max placed nodes = 105`

Comparison:

- same run at `cost=0`: `105`

Conclusion:

- At least for the tested seed and short horizon, cost objective `0` vs `1` does not materially change feasibility.
- The search is failing before cost objective meaningfully differentiates complete solutions.

### 12. Probe seed-dependent variant quality

At one point, a seed-selected 1-SSG variant experiment was tried. The idea was:

- keep one deep search,
- but change which network/order variant that single search uses based on `seed`.

Short probe over seeds `0..7` at `t=10` gave:

- `0 -> 83`
- `1 -> 16`
- `2 -> 77`
- `3 -> 16`
- `4 -> 83`
- `5 -> 39`
- `6 -> 92`
- `7 -> 20`

Interpretation:

- `co_to_ci` forward-placement variants were clearly the only promising families.
- `ci_to_co`-style variants were consistently poor.

However, after trying a seed-selected 1-SSG implementation:

- some previously decent seeds got worse
- this did not beat the simpler “always use breadth `co_to_ci`” large-instance fallback

Conclusion:

- The experiment was useful diagnostically.
- The final code does not keep the seed-selected 1-SSG variant approach.

## Current Code State

### Kept changes

1. Per-SSG depth tracking

- `search_space_graph::max_placed_nodes` in [`graph_oriented_layout_design.hpp`](/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp)

2. Earlier native-hex improvements from this broader line of work, still present

- PI x-ordering bias based on upcoming consumer anchoring
- better wide-PI candidate ordering
- explicit pin-order lists treated as soft preferences rather than hard up-front PI scheduling rewrites
- reduced penalty weight for explicit PI/PO order lists
- bounded maximum-effort randomized SSG generation

3. Large-instance native-hex maximum-effort now collapses to a single deep SSG

- for `num_pis >= 32` and `num_gates >= 150`

4. Regression coverage for that large-instance behavior

- [`graph_oriented_layout_design_hex.cpp`](/home/samuelshng/git/fiction-sam/test/algorithms/physical_design/graph_oriented_layout_design_hex.cpp)

### Not kept conceptually

These were explored and should not be repeated blindly:

- pilot-and-prune large-instance strategy
- making large native-hex runs prefer declaration-order PI/PO placement
- increasing `g` from `6` to `7` on `w4a4` without other changes
- increasing `num_vertex_expansions` to `6` or `8` as a first-line fix
- seed-selected 1-SSG variant routing without stronger evidence

## High-Level Findings

### 1. The main bug found on this date was a real crash in the large-instance native-hex path

This was not a minor issue:

- the reduced-SSG large-instance path wrote beyond `ssg_vec`
- targeted `w4a4` reruns were exiting with `rc=139`
- that is fixed now

So one concrete outcome of this exploration is that large native-hex `gold` is more robust than before.

### 2. The best-performing large-instance strategy so far is still one deep search-space graph

For `w4a4`, the current best measured state is:

- `seed=81`
- `g=6`
- `-j`
- `-t 400`
- `cost=0`
- `--grid hex`
- `num search space graphs = 1`
- `max placed nodes = 338`

This is better than the repaired pilot path, but still not enough to finish.

### 3. Pin-order preference is not a general answer

Important split:

- `w2a2`: soft PI/PO order preference can help
- `w4a4`: soft PI/PO order preference is consistently harmful in the tested runs

So native-hex interface-order heuristics are benchmark-sensitive. Anything future here should be conditional, not global.

### 4. `w4a4` is still failing because the search commits to a branch that grows depth but does not reach closure

The evidence for this is:

- one-SSG depth continues improving from `30s -> 120s -> 400s`
- but still never finds a full layout
- pilot pruning plateaued entirely
- extra `g`, extra `n`, and alternate cost did not help

This looks less like “search budget too small” and more like “the branch ordering is wrong once the layout gets large.”

## What Seems To Be Lacking

My current view is that native hex is not primarily missing another cheap scalar knob. It is missing a stronger late-stage search policy for large wide-frontier networks.

The likely gaps are:

### 1. Better large-instance branch selection after the early PI/frontier phase

The current search can make it to `338` placed nodes on `w4a4`, which is far past the initial PI setup.

That suggests the remaining issue is likely around:

- gate ordering after the wide-frontier opening,
- PO placement/routing preparation,
- or the choice of local geometric branch once the first half of the network is embedded.

### 2. Stronger large-instance diversity without splitting the timeout into many shallow SSGs

This exploration tried:

- many shallow SSGs: too shallow
- pilot then prune: chooses the wrong survivor
- one deep SSG: best so far, but still can get stuck

What is still missing is a way to preserve depth while still switching out of a bad branch later in the run.

### 3. Better understanding of where placement depth stalls

`max placed nodes` tells us how far the search got, but not:

- which node types were last being placed,
- whether failures cluster around POs,
- whether the search stalls after certain fanout-substitution structures,
- whether the final dead end is geometric congestion or ordering pathology.

Without that, heuristic changes are still partially blind.

## Recommended Next Steps

These are the next steps I would recommend in priority order.

### 1. Add lightweight late-stage instrumentation for large native-hex runs

Do this before any larger heuristic rewrite.

Specifically, log for large native-hex runs:

- node index at stall
- node type of the current/last expanded node
- whether current node is PI / gate / PO
- fanin span of the current node
- which topological order/fanout-substitution variant is active
- how often candidate generation returns zero positions for PI / gate / PO separately

This should make it clear whether `w4a4` is stalling:

- near PO launch/collection,
- on wide 2-input gate routing,
- or on some specific topological-order interaction.

### 2. Instrument and compare “deepest failed partial layouts”

For the best failed seeds:

- dump the partial layout or enough metadata to identify the last placed region
- compare the best failed native-hex state against a successful cartesian `w4a4` run structurally

This should answer:

- whether native hex is failing in the same general network region across seeds
- or whether failures are diffuse and purely heuristic

### 3. Try controlled branch restarts within one deep run

This is the most promising algorithmic direction from the current evidence.

The key idea:

- do not pay for 16 or 32 shallow SSGs from the start
- but also do not stay trapped forever in one deep branch

A plausible next design:

- start with one deep SSG
- when progress plateaus for a while, fork from a recent checkpoint into 2-3 alternative continuations
- keep only the best continuation after a bounded budget

This is different from the pilot-prune attempt:

- pilot-prune made the decision too early
- this would diversify only after enough structure has already been placed

### 4. Focus variant diversity only on the forward `co_to_ci` families

The 8-seed probe strongly suggested:

- `ci_to_co` variants are poor for this benchmark
- `co_to_ci` variants are the only ones worth deeper investment

So any future large-instance diversity mechanism should prioritize:

- breadth `co_to_ci`
- depth `co_to_ci`
- random fanout-substitution `co_to_ci`

and deprioritize or omit `ci_to_co` on `w4a4`-like large native-hex runs.

### 5. Do not spend more time on these without new evidence

Unless instrumentation shows otherwise, the following have already been checked and should not be the next move:

- turning on declaration-order `--prefer_input_pin_order --prefer_output_pin_order`
- simply increasing `g`
- simply increasing `num_vertex_expansions`
- changing cost objective between `0` and `1`
- feeding in cartesian-derived explicit pin orders and expecting that alone to solve it

## Current Best Known Practical State

If someone wants to reproduce the current best measured native-hex `w4a4` behavior from this exploration, use:

```text
read -a /home/samuelshng/git/verilog-mxu/collab_ext/w4a4_tnano/w4a4_deepsyn3652.v
map --all2
gold -e 3 -s 81 -g 6 -j -t 400 -c 0 --grid hex
ps -g
```

Measured result on the current tree:

- success: no layout
- `max placed nodes = 338`
- `num search space graphs = 1`
- runtime about `400.5s`

This is not a success, but it is the strongest native-hex `w4a4` result reached during this day’s work.

## Summary

What was achieved:

- fixed a real native-hex large-instance crash
- established that the pilot-prune idea is stable only after fixing that bug, but still inferior
- established that one deep SSG is the best current large-instance native-hex strategy
- established that soft pin-order preference helps `w2a2` but hurts `w4a4`
- established that `g`, `n`, and cost tweaks are not the missing answer
- pushed `w4a4` native-hex depth substantially farther than the original all-fail state

What was not achieved:

- `w4a4` native-hex still does not place and route successfully

What should happen next:

- add targeted stall instrumentation
- use that to design a late-branch-diversification strategy for large native-hex runs
- keep the large-instance search deep by default

## 2026-03-15 Addendum: Stall Diagnostics

This addendum records the next exploration step after the original note: add lightweight failure diagnostics to native-hex GOLD and use them on targeted `w4a4` reruns.

### Diagnostic instrumentation added

The native-hex GOLD stats now report:

- zero-candidate failures split by PI / gate / PO
- route failures split by gate / PO
- invalid-layout prune count
- deepest failed node with:
  - node kind
  - node function
  - failure reason
  - failure detail
  - SSG label

The intent was to answer whether `w4a4` was really stalling on:

- empty candidate generation,
- route failures,
- PO pressure,
- or structural feasibility pruning.

### Targeted `w4a4` results

All runs below used:

```text
read -a /home/samuelshng/git/verilog-mxu/collab_ext/w4a4_tnano/w4a4_deepsyn3652.v
map --all2
gold -e 3 -g 6 -j -t 30 -c 0 --grid hex
```

#### Seed 81

- `max placed nodes = 105`
- `num search space graphs = 1`
- `invalid layout prunes = 13`
- `zero candidate pi/gate/po = 0`
- `route failures gate/po = 0`
- deepest failure:
  - function: `fanout`
  - reason: `invalid_layout`
  - detail: `fanout_missing_dual_exits`
  - placed nodes: `96`
  - SSG: `breadth_co_to_ci`

#### Seed 93

- `max placed nodes = 105`
- `num search space graphs = 1`
- `invalid layout prunes = 13`
- `zero candidate pi/gate/po = 0`
- `route failures gate/po = 0`
- deepest failure:
  - function: `fanout`
  - reason: `invalid_layout`
  - detail: `fanout_missing_dual_exits`
  - placed nodes: `96`
  - SSG: `breadth_co_to_ci`

#### Seed 66

- `max placed nodes = 100`
- `num search space graphs = 1`
- `invalid layout prunes = 38`
- `zero candidate pi/gate/po = 0`
- `route failures gate/po = 0`
- deepest failure:
  - function: `fanout`
  - reason: `invalid_layout`
  - detail: `no_bottom_path`
  - placed nodes: `100`
  - SSG: `breadth_co_to_ci`

### What this changes in the diagnosis

This is much more specific than the original note.

The current native-hex `w4a4` problem is not primarily:

- PO routing
- zero candidate generation
- or route failure during gate placement

At least for the sampled strong seeds, the search is dying because already placed fanout nodes become structurally invalid under native-hex feasibility checks.

The two observed failure details were:

- `fanout_missing_dual_exits`
- `no_bottom_path`

So the current best interpretation is:

- the search gets far enough to place a substantial fanout-heavy partial layout
- later branch choices make one or more unresolved fanout nodes lose their required future escape capacity
- `valid_layout` then prunes the branch

This also explains why changing scalar knobs like `g`, `n`, and cost objective was mostly ineffective: the main issue is not search breadth in the abstract, but how the search preserves unresolved fanout feasibility.

### Heuristics tried on 2026-03-15

#### 1. PI-only feasibility filter

Tried:

- when enumerating PI candidates, reject candidates that already fail the existing feasibility checks for the current partial layout

Observed result on seed `81`, `t=30`:

- baseline: `max placed nodes = 105`
- with filter: `106`

Interpretation:

- directionally plausible, but too small to call a meaningful improvement

Status:

- not kept as a claimed fix

#### 2. Broader candidate filter for unresolved launch tiles

Tried:

- reject candidate placements that would block unresolved fanout / multi-output launch tiles during candidate generation for PI and gate placements

Observed result on seed `81`, `t=30`:

- regressed from `105` to `43`
- `invalid layout prunes` exploded to `6514`

Deepest failure in that bad version:

- function: `fanout`
- reason: `invalid_layout`
- detail: `no_bottom_path`

Interpretation:

- this heuristic was too blunt
- it over-constrained the search and pushed it into a much worse region

Status:

- reverted

### 2026-03-15 Addendum B: schedule-context diagnostics

I extended the failure snapshot again to capture:

- frontier node kind/function
- frontier PI run length
- failing node driver kind/function
- failing node placed successors / total successors

This was done because “the offending node is a fanout” still left an important ambiguity:

- is the search getting stuck behind a large late PI block, or
- is it losing fanout-tree feasibility even when only one isolated PI remains before the next gate?

#### Re-run results with schedule context

##### Seed 81

- `max placed nodes = 105`
- deepest failure:
  - node: `fanout`
  - detail: `fanout_missing_dual_exits`
  - frontier: `pi`
  - frontier PI run length: `1`
  - driver: `fanout`
  - placed successors: `0 / 2`

##### Seed 93

- `max placed nodes = 105`
- deepest failure:
  - node: `fanout`
  - detail: `fanout_missing_dual_exits`
  - frontier: `pi`
  - frontier PI run length: `1`
  - driver: `fanout`
  - placed successors: `0 / 2`

##### Seed 66

- `max placed nodes = 100`
- deepest failure:
  - node: `fanout`
  - detail: `no_bottom_path`
  - frontier: `fanout`
  - frontier PI run length: `0`
  - driver: `fanout`
  - placed successors: `1 / 2`

#### What changed in the diagnosis

This rules out one of the more obvious hypotheses.

The strong `w4a4` seeds are **not** dying because a long tail of late PIs is queued ahead of the remaining logic. In the two strongest samples, the frontier PI run length is only `1`. So the search is already down to an isolated PI immediately before its consumer, and the unresolved upstream fanout still loses its two-exit feasibility.

The more accurate interpretation is now:

- the hard cases are centered on **fanout chains**
- the offending fanouts are themselves driven by other fanouts
- seed `81` / `93` fail before either successor of the offending fanout has been placed
- seed `66` fails later, after one successor has already been placed and the next frontier node is another fanout

So the bottleneck is narrower than “PI scheduling”:

- it is fanout-tree feasibility in the presence of late local routing choices
- especially preserving future exits for fanout-to-fanout regions

#### Targeted heuristic tried after this diagnosis

Tried:

- for large-instance native-hex runs only, when the frontier is an isolated PI with an anchored preferred `x`, keep only a narrow band of PI candidates nearest that preferred `x`

Reasoning:

- if the frontier PI is isolated rather than part of a long PI block, perhaps the search is wasting time exploring the full top border when it should focus around the already anchored sibling/consumer region

Observed results:

- seed `81`: `max placed nodes` improved from `105` to `107`
- seed `93`: unchanged at `105`
- seed `66`: unchanged at `100`

Interpretation:

- this was at best a weak local nudge
- it did not change the dominant failure mode
- it was not strong enough to keep

Status:

- reverted

### Updated near-term recommendation

The next promising work should focus specifically on unresolved **fanout-chain** handling, not generic search tuning.

The strongest next candidates are:

1. Fanout-aware branch selection or scheduling.
   The evidence now points to fanout-to-fanout regions as the real bottleneck, not broad PI ordering.

2. Add a surgical heuristic for preserving future exits of unresolved fanout chains.
   This should be narrower than the reverted launch-blocking filter and more structural than simple PI candidate narrowing.

3. Compare the failing fanout-chain region against a successful cartesian `w4a4` partial / full run.
   The current diagnostics are now specific enough to make that comparison meaningful.

4. Consider a search-space variant that explicitly favors resolving fanout successors once a fanout chain is opened.
   The schedule-context data suggests that leaving a fanout chain partially unresolved is what pushes these branches over the edge.

## 2026-03-15 addendum 3: blocked fanout exits are not the fanout's own incoming route

### Motivation

The previous addendum narrowed the dominant `w4a4` native-hex failure to `fanout_missing_dual_exits`, usually on a fanout with `0 / 2` successors already placed.

At that point there were two plausible interpretations:

1. the fanout's own incoming route was entering from `south_west` or `south_east` and consuming one of its future exits
2. unrelated nearby routing had already occupied one of those future exits before or while the fanout was placed

Those imply different fixes, so the next work focused on separating them.

### Experiment 1: reject fanout/multi-output routes that enter through reserved future exits

Tried:

- add a geometric legality check in native-hex `route_single_input_node` / `route_double_input_node`
- for fanouts and multi-output gates, reject routes whose final step into the sink comes through one of the sink's projected future output exits

Reasoning:

- if the failing `south_west` wire is actually the fanout's own incoming edge, this should eliminate that class of invalid partial layouts early

Observed results on representative `w4a4` seeds (`30s`, `g=6`, `-j`, `cost=0`):

- seed `81`: unchanged, `max placed nodes = 105`
- seed `93`: unchanged, `max placed nodes = 105`
- failure detail stayed `fanout_missing_dual_exits sw=wire se=empty`

Interpretation:

- the blocked `south_west` tile is not explained by the sink's own final approach edge
- this hypothesis did not move the search and was not kept

Status:

- reverted

### Experiment 2: add direction detail to the failing fanout-exit diagnostics

Added:

- richer failure detail for `fanout_missing_dual_exits`
- the detail now records whether the blocked `south_west` or `south_east` tile is directly incoming to the failing fanout:
  - `sw_in=0/1`
  - `se_in=0/1`

Representative reruns:

- seed `81`
- seed `93`

Observed results:

- seed `81`: `fanout_missing_dual_exits sw=wire se=empty sw_in=0 se_in=0`
- seed `93`: `fanout_missing_dual_exits sw=wire se=empty sw_in=0 se_in=0`

Interpretation:

- the blocked exit is **not** the fanout's own incoming route
- the fanout dies with an unrelated wire already occupying `south_west`
- the dominant issue is therefore not "wrong sink approach direction"

Status:

- kept

### Experiment 3: protect reserved exits of already dangling fanouts from later unrelated routes

Tried:

- reject any path that traverses the projected `south_west` / `south_east` exits of an already placed dangling fanout
- exempt the path's own source fanout so genuine fanout launches are still allowed

Reasoning:

- if later unrelated routing is trampling those reserved exits, protecting them globally during routing should change the failure signature or increase depth

Observed results on representative `w4a4` seeds:

- seed `81`: unchanged, `max placed nodes = 105`
- seed `93`: unchanged, `max placed nodes = 105`
- failure detail still `fanout_missing_dual_exits sw=wire se=empty sw_in=0 se_in=0`

Interpretation:

- the dominant blocking wire is not being introduced by a later route that this guard can intercept
- the problematic congestion is already present when the failing fanout candidate is considered
- equivalently: this is not just "later routes trample previously reserved exits"

Status:

- reverted

### Updated interpretation after addendum 3

The strongest current interpretation is now:

- the critical `w4a4` native-hex failure is still centered on **fanout chains**
- but the decisive blockage is already present **before** the failing fanout is accepted
- the blocked `south_west` tile is:
  - a real wire at `z=0`
  - not the fanout's own incoming edge
  - not removed by rejecting later routes through reserved exits of dangling fanouts

So the next work should stop targeting route-finalization details and instead focus on:

1. why candidate fanout placements are being considered in regions that already have reserved-exit congestion
2. whether the congestion comes from earlier branch-routing choices that are locally valid but globally hostile to downstream fanout expansion
3. whether native-hex search needs explicit look-ahead or scoring around *future fanout exits* before the failing fanout is placed

### Recommended next step from here

The best next experiment is no longer another routing guard.

The next productive move should be one of:

1. compare the failing native-hex fanout-chain neighborhood against a successful cartesian `w4a4` layout around the analogous chain
2. log the local neighborhood of the failing fanout candidate itself (tile coordinates plus nearby occupied gate/wire/fanout tiles) so the hostile geometry is visible directly
3. introduce a candidate scorer for single-input fanouts based on nearby reserved-exit clearance *before* placement acceptance, rather than rejecting routes after the fact

## 2026-03-15 addendum 4: distinct-exit filtering is the first native-hex `w4a4` improvement that clearly moves the needle

### New observation from neighborhood dumps

After adding local context to `fanout_missing_dual_exits`, the strongest representative seed (`81`) exposed a concrete structural issue:

- failure detail:
  - `fanout_missing_dual_exits center=(0, 15) sw=(0, 15)=wire se=(0, 16)=empty sw_in=0 se_in=0`
  - local context:
    - `ctx=[(+0,-1)=wire,(+1,-1)=wire,(+2,-1)=wire,(+0,+0)=wire,(+2,+0)=wire,(+2,+1)=wire,(+0,+2)=wire]`

The critical detail is not the wire pattern but the coordinates:

- the failing fanout candidate sits on the **left boundary**
- its projected `south_west` exit collapses back onto the same tile (`sw=(0, 15)`)

That means native hex was still considering fanout / 2-output placements that can never have two distinct future exits, regardless of routing quality.

### Fix tried: reject fanout / 2-output positions without distinct projected exits

Implemented:

- during candidate generation for native-hex single-input and double-input nodes
- if the current node is a fanout or other 2-output node, reject candidate positions whose projected `south_west` / `south_east` exits are not both distinct and different from the candidate tile

Reasoning:

- this is a structural impossibility, not a heuristic preference
- pruning these candidates early should remove wasted search without repeating the earlier over-aggressive route-based filters

### Observed impact

Representative results on seed `81`, `g=6`, `-j`, native hex:

- before this filter:
  - `30s`: `max placed nodes = 105`, deepest failure `fanout_missing_dual_exits`
  - `60s`: `max placed nodes = 140`, deepest failure `zero_candidates`
- after this filter:
  - `30s`: `max placed nodes = 109`, deepest failure `no_bottom_path`
  - `120s`: `max placed nodes = 230`, deepest failure `fanout_missing_dual_exits center=(71, 3) sw=(70, 4)=fanout se=(71, 4)=empty ...`
  - `400s`: `max placed nodes = 340`, deepest failure `no_bottom_path`

Important comparison:

- an earlier long native-hex run on this seed plateaued at `338`
- the new boundary-only filter reaches `340`

So this is not just noise:

- it removes one real class of impossible candidate placements
- it changes the deep-search failure mode
- it is the first native-hex `w4a4` heuristic change in this exploration that clearly improves the strongest sampled seed instead of merely changing diagnostics

### Follow-up that did *not* help

After the `120s` run exposed a later failure where one reserved exit was occupied by another **fanout**:

- `fanout_missing_dual_exits center=(71, 3) sw=(70, 4)=fanout se=(71, 4)=empty ...`

I tried a second filter:

- reject native-hex fanout / 2-output candidates whose reserved exits are already occupied by a structural non-wire blocker

Result:

- clean `60s` rerun of seed `81` dropped back to `max placed nodes = 136`

Interpretation:

- this second filter was too aggressive
- it likely cut off branches that were still worth exploring under the current limited candidate budget

Status:

- reverted

### Current best interpretation

The search is now getting farther because one clearly impossible class of fanout positions is gone, but `w4a4` still does not complete.

The current best understanding is:

1. native hex was wasting search on structurally impossible left-boundary fanout / 2-output placements
2. removing those placements helps materially
3. the next bottleneck is no longer the same boundary trap
4. deeper in the search, the remaining failures shift between:
   - `fanout_missing_dual_exits` caused by other placed fanouts or congestion near the reserved exits
   - `no_bottom_path`

### Best next step after this addendum

The next useful work should focus on the *new* late-stage bottlenecks, not the already-fixed boundary case.

Most promising:

1. extend the local-neighborhood dump for `no_bottom_path` and inspect the first reliable post-filter failure context
2. compare that late-stage geometry against a successful cartesian `w4a4` layout region
3. consider scoring fanout candidates by future-exit clearance rather than filtering them hard, since the second hard filter regressed search depth

## 2026-03-15 addendum 5: failed follow-up heuristics after the boundary-only fix

After the boundary-only distinct-exit filter, I tried several additional ideas. None of them improved on the current best native-hex baseline, and several regressed badly.

### 1. Exit-path validity checks for unresolved fanout / 2-output exits

Idea:

- strengthen `valid_layout` so that unresolved fanout / 2-output exits must not only be locally available, but must also already have their own bottom-row path

Why it seemed plausible:

- the late failures were repeatedly `no_bottom_path` on fanouts with only one of two successors placed
- this suggested the remaining projected exit might already be boxed in even when the source tile still looked vaguely legal

What happened:

- on representative `w4a4` seeds, this mostly just converted the failure detail into
  - `fanout_remaining_exit_no_bottom_path ...`
- but it did not improve search depth
- it also started to over-prune smaller `w2a2` spot checks under short timeouts

Status:

- reverted

### 2. Route-level retry for fanout launch legality

Idea:

- native hex already enforced launch-side legality during routing for true 2-output gates
- it did **not** do that for plain fanouts
- I modified `check_path` so that if a routed fanout branch chose an illegal immediate launch, the algorithm would obstruct that first step and retry A* a few times

Why it seemed plausible:

- otherwise the second branch of a fanout can still route through the already-used launch side and only be rejected later in `valid_layout`

Measured result:

- seed `81`, `60s`, `g=6`, `-j`, native hex:
  - baseline boundary-only variant: `max placed nodes = 140`
  - with fanout launch retry: `max placed nodes = 136`

Status:

- reverted

### 3. Large-instance left-boundary filters

I tried two variants.

#### 3a. Reject `x = 0` only for fanout / 2-output candidates

Motivation:

- many failing native-hex partial layouts still died on left-edge fanouts

Measured result:

- seed `81`, `60s`: still `136`
- no improvement over the boundary-only baseline

Status:

- reverted

#### 3b. Reject `x = 0` for all internal gate candidates on large native-hex instances

Motivation:

- I inspected a successful **cartesian-derived hex** `w4a4` layout:
  - `/home/samuelshng/git/verilog-mxu/gold_sweep_results_2i2o_20260309_test_01/artifacts/gold_sweep_results_w4a4_g6_cost1/seed_00057/hex.fgl`
- that working hex layout has:
  - minimum gate `x = 1`
  - no cells at `x = 0`
  - only `BUF` cells near the far-left edge

This suggested that native hex might also benefit from banning internal gate placements on the extreme left boundary.

Measured result:

- seed `81`, `60s`: `max placed nodes = 114`

This was a clear regression.

Status:

- reverted

### 4. Alternative single-SSG orderings for the large-instance fallback

Current large-instance native hex does **not** actually use pilot-and-prune at the moment:

- `should_use_search_depth_pilot()` returns `false`
- `should_prioritize_search_depth()` immediately collapses large instances to one deep SSG
- that one deep SSG is hardwired to `breadth_co_to_ci`

I tested whether the large-instance failure might simply be the wrong one-SSG choice.

#### 4a. `depth_co_to_ci`

Measured result:

- seed `81`, `60s`: `max placed nodes = 138`

This is slightly worse than the `breadth_co_to_ci` boundary-only baseline (`140`).

#### 4b. `breadth_ci_to_co`

Measured result:

- seed `81`, `60s`: `max placed nodes = 16`

This is catastrophically worse.

Conclusion from these order experiments:

- the current failure is not explained by “the wrong single SSG ordering”
- `breadth_co_to_ci` remains the best of the tested one-SSG large-instance options so far

Status:

- reverted back to `breadth_co_to_ci`

### 5. Inflate native-hex gate candidate budgets for large instances

Idea:

- double the native-hex single-input and double-input expansion budgets only when `should_prioritize_search_depth()` is active

Why it seemed plausible:

- many late `w4a4` failures were `zero_candidates`
- that suggested candidate starvation rather than pure invalid-layout pruning

Measured result:

- seed `81`, `60s`: `max placed nodes = 114`

Again, this was much worse than the boundary-only baseline.

Status:

- reverted

## Current best state after addendum 5

The only `w4a4`-specific native-hex change from this exploration that is still worth keeping is the earlier:

- reject fanout / 2-output candidate positions without distinct projected `south_west` / `south_east` exits

Everything else in this addendum failed to improve on that baseline.

So the current representative picture remains:

- baseline with boundary-only distinct-exit filter:
  - seed `81`, `60s`: `140`
  - seed `81`, `120s`: `230`
  - seed `81`, `400s`: `340`
- failed follow-up tweaks:
  - fanout launch retry: `136`
  - fanout-only `x=0` filter: `136`
  - all-gates `x=0` filter: `114`
  - doubled candidate budget: `114`
  - one-SSG `depth_co_to_ci`: `138`
  - one-SSG `breadth_ci_to_co`: `16`

## Updated interpretation

The strong conclusion from this round is:

1. the native-hex `w4a4` bottleneck is **not** fixed by simple boundary bans, simple launch-side retries, simple candidate-budget inflation, or simply swapping the one deep SSG ordering
2. the remaining issue is likely a more structural search-design limitation in how native hex handles deep fanout-heavy regions after the initial boundary trap is removed
3. the best next work should be diagnostic / structural, not another batch of blind scalar heuristic tweaks

## Best next step now

The highest-value next step is to compare **working** geometry against the failing native-hex geometry:

1. inspect the successful cartesian-derived hex `w4a4` layout in more detail
2. quantify where its buffer/fanout analogues, branch corridors, and left-edge occupancy actually sit
3. compare that against the local neighborhoods captured by native-hex failures such as:
   - `fanout_missing_dual_exits`
   - `no_bottom_path`
4. use that comparison to design a targeted heuristic or reservation rule grounded in a working layout shape, rather than another broad search tweak

## 2026-03-15 addendum 6: switch from single-seed anecdotes to a fixed 15-seed physical-core batch

The next exploration step changed the evaluation method.

Up to this point, many heuristic attempts had been judged on one or two representative seeds such as `81`, `93`, and
`66`. That was useful for diagnosis, but it was too easy to overfit on a single run. The next pass therefore used a
fixed 15-seed batch, with each `fiction` process kept single-threaded and pinned to a distinct physical core so that
15 runs could be evaluated in parallel without SMT contention dominating the result.

### Batch setup

- host CPU topology:
  - 16 physical cores
  - 32 logical CPUs
  - chosen physical-core logical CPUs for this batch: `0..14`
- fixed seed sample:
  - `72, 80, 18, 102, 3, 56, 28, 100, 65, 29, 71, 83, 103, 52, 66`
- command per seed:

```text
read -a /home/samuelshng/git/verilog-mxu/collab_ext/w4a4_tnano/w4a4_deepsyn3652.v
map --all2 -v
gold -e 3 -s <seed> -g 6 -j -t 60 -c 0 --grid hex
quit
```

The seed sample is stratified from the old `w4a4 g6 cost0` sweep depths rather than purely random. That makes it
more representative than a single-seed test while still staying focused on seeds that span weak, medium, and stronger
historical behavior.

### Baseline batch result

With the retained native-hex baseline at this point in the exploration (including the earlier boundary-only distinct
projected-exit filter, but without any new heuristic from this addendum), the 15-seed batch produced:

- mean `max placed nodes`: `109.8`
- median-like sorted sample midpoint: `100`
- minimum `max placed nodes`: `100`
- maximum `max placed nodes`: `136`
- seeds `>= 150`: `0 / 15`

Observed pattern:

- most seeds collapsed into an early repeated `fanout` failure around the far-left region
- the dominant detail for those was:
  - `no_bottom_path center=(3, 5) ...`
- a smaller subset instead reached later `xor` zero-candidate failures:
  - seeds `56`, `80`, `103`
  - depths `135`, `136`, `136`

So the batch result is harsher than the hand-picked single-seed story. That is useful because it reveals that some
changes that looked neutral or mildly promising on a strong seed are actually poor on a broader slice of the search
distribution.

### One diagnostic detour: zero-candidate cause on the active `xor` wall

Before testing the next heuristic, I temporarily instrumented zero-candidate failures to see why the active `xor`
frontier node had no feasible positions.

Representative diagnostic on seed `81`, `60s`:

- `fanin_count=2 checked=720 accepted=0 distinct_exit=0 first_path=549 second_path=171 bottom_path=0`

Interpretation:

- the active zero-candidate wall was **not** a bottom-row feasibility issue at that point
- it was pure double-fanin gate-routing starvation
- candidate positions were being rejected on first- or second-fanin routing before drain reachability even mattered

I removed that diagnostic helper afterward because the extra work inside every zero-candidate branch distorted
timeout-limited comparisons.

### Heuristic A/B: alternate routing order for 2-input gates

Hypothesis:

- native hex always routed double-fanin gates in one fixed fanin order
- if the first chosen path consumed the critical corridor, the second path could fail even though the reverse order
  might have worked

I implemented this in two places so the search and actual placement would stay aligned:

1. candidate generation for 2-input gates in native hex
2. actual `route_double_input_node`

Then I reran the exact same 15-seed, 15-core batch.

### Alternate-order batch result

This change was a regression.

- mean `max placed nodes`: `102.6`
- median-like sorted sample midpoint: `101`
- minimum `max placed nodes`: `99`
- maximum `max placed nodes`: `109`
- seeds `>= 150`: `0 / 15`

Compared with the baseline batch:

- mean dropped from `109.8` to `102.6`
- maximum dropped from `136` to `109`
- several seeds regressed into earlier far-left `fanout_missing_dual_exits` / `no_bottom_path` failures

So although the fixed-order routing looked suspicious in the abstract, reversing or trying both orders for double-fanin
gates does **not** help `w4a4` at the batch level. It was reverted.

## Updated takeaway after addendum 6

Two important process-level conclusions came out of this round:

1. future heuristic work on native-hex `w4a4` should be judged on a fixed multi-seed physical-core batch, not on one
   favored seed
2. the current `w4a4` wall is still dominated by early far-left fanout failures across the broader distribution, even
   if strong individual seeds can get much farther

And one concrete technical conclusion:

- alternate routing order for 2-input gates is not the missing fix; it regresses the 15-seed batch and should stay
  out

## Best next step after addendum 6

The next heuristic should target the **early repeated far-left fanout collapse** seen across the batch, not just the
late strong-seed `xor` wall.

That suggests two better directions than the reverted alternate-order change:

1. explicitly characterize why so many batch seeds converge on nearly the same early fanout geometry near the left
   edge
2. design a targeted placement or reservation heuristic for those early fanout chains, then evaluate it immediately on
   the same fixed 15-seed pinned batch

## 2026-03-15 addendum 7: left-margin fanout placement filters after the 15-seed batch switch

This addendum continues directly from addendum 6 and keeps the same evaluation method:

- fixed 15-seed batch
- one single-threaded `fiction` process per physical core
- same `w4a4`, `g=6`, `-j`, `-t 60`, `cost=0`, native-hex command
- same baseline for comparison:
  - mean `109.8`
  - max `136`
  - dominant failure: early far-left `fanout` collapse around `center=(3, 5)`

The next experiments all targeted the same concrete hypothesis:

- native hex is still willing to place fanout / multi-output nodes too far left once the search reaches deeper rows
- those placements are locally legal enough to survive candidate generation, but they correlate strongly with the
  repeated early `fanout_missing_dual_exits` / `no_bottom_path` wall seen across the batch

### Experiment 1: broad left-margin filter for fanout / multi-output positions at `x < 4`

First attempt:

- reject native-hex candidate positions for `fanout` and generic multi-output nodes when:
  - `should_prioritize_search_depth()`
  - `new_pos.x < 4`
- this was intentionally broad and was meant only as a quick probe of whether the left-most columns were the real
  problem

Outcome:

- it improved the batch sharply, which confirmed the hypothesis
- but it was too aggressive and visibly hurt stronger representative seeds

I did not keep this exact variant.

### Experiment 2: broad left-margin filter at `x < 4` but only from row `y >= 4`

Next attempt:

- keep the same broad `fanout` / multi-output filter
- but only apply it once the search is at least a few rows below the PI frontier:
  - `new_pos.x < 4 && new_pos.y >= 4`

This was tested on the same 15-seed pinned batch:

- output directory:
  - `/tmp/w4a4_batch_xlt4_y4fanout_20260315`
- batch result:
  - mean `max placed nodes`: `130.86666666666667`
  - midpoint: `135`
  - minimum: `120`
  - maximum: `137`
  - seeds `>= 150`: `0 / 15`

Failure distribution shifted materially:

- `12 / 15` seeds now died later at `zero_candidates` on `xor`
- only `3 / 15` remained in `invalid_layout` on `fanout`

This is the best batch result reached so far in the exploration.

Tradeoff:

- it still hurts strong seeds
- seed `81`, run at `120s`, reached only `120` placed nodes on one direct check and then `176` on a rerun with the
  same heuristic family; both are below the earlier stronger single-seed results reached before the batch-guided
  filter work

Interpretation:

- this heuristic is broad enough to help the weak and medium part of the distribution
- but it also suppresses some later placements that stronger seeds were using productively

### Experiment 3: widen the left-margin filter to `x < 5`

Hypothesis:

- maybe the search still needed a larger forbidden left strip

Tested variant:

- broad `fanout` / multi-output filter
- `new_pos.x < 5`

Output directory:

- `/tmp/w4a4_batch_xlt5fanout_20260315`

Outcome:

- mean `123.73333333333333`
- max `136`

This was worse than the `x < 4` version and was reverted immediately.

### Experiment 4: narrow the filter to deeper fanout-on-fanout chains only

Because the broad `x < 4 && y >= 4` filter helped the batch but hurt strong seeds, I then narrowed the rule to the
most suspicious local shape:

- current node is `fanout`
- driver is also `fanout`
- `new_pos.x < 4 && new_pos.y >= 4`
- remove the corresponding restriction from the double-fanin path entirely

Batch run:

- output directory:
  - `/tmp/w4a4_batch_fanoutchain_xlt4_y4_20260315`
- batch result:
  - mean `128.86666666666667`
  - midpoint: `128`
  - minimum: `119`
  - maximum: `136`

Strong-seed check:

- output file:
  - `/tmp/w4a4_seed81_fanoutchain_xlt4_y4_t120.json`
- seed `81`, `120s`:
  - `max placed nodes = 195`

Interpretation:

- this is a real compromise
- it recovers some stronger-seed behavior
- but it gives back too much of the batch gain

I did not keep this version because the broader `x < 4 && y >= 4` rule still gives the best aggregate batch result.

### Experiment 5: delay the broad filter to `y >= 5`

This was a clean follow-up to test whether the broad rule was simply kicking in one row too early:

- broad `fanout` / multi-output filter
- `new_pos.x < 4 && new_pos.y >= 5`

Batch run:

- output directory:
  - `/tmp/w4a4_batch_xlt4_y5_20260315`
- batch result:
  - mean `129.53333333333333`
  - midpoint: `128`
  - minimum: `120`
  - maximum: `136`

Strong-seed check:

- output file:
  - `/tmp/w4a4_seed81_xlt4_y5_t120.json`
- seed `81`, `120s`:
  - `max placed nodes = 176`

Interpretation:

- delaying the filter by one row does not recover strong-seed depth
- and it is still worse than the `y >= 4` batch winner

This variant was reverted.

## Updated takeaway after addendum 7

One concrete native-hex heuristic is now worth keeping:

- for large-instance search-depth mode, reject `fanout` / multi-output candidate positions in the far-left strip once
  the search is at least four rows deep:
  - `new_pos.x < 4 && new_pos.y >= 4`

Why keep it:

- it is the best 15-seed batch result reached so far
- it moves the dominant failure away from the old repeated left-edge fanout collapse and into later `xor`
  zero-candidate starvation

Why this is still not enough:

- it does not yield a successful `w4a4` layout
- it still suppresses some stronger-seed trajectories
- the active wall after this change is later and narrower, but still real

## Best next step after addendum 7

The batch evidence now says the left-edge fanout trap is only one layer of the problem. After the retained
`x < 4 && y >= 4` filter, the dominant wall becomes later `xor` zero-candidate starvation.

So the next work should move to that later wall:

1. instrument one representative post-filter `xor` zero-candidate case again, but now on the batch-winning heuristic
2. determine whether the remaining starvation is mostly:
   - first-fanin path failure
   - second-fanin path failure
   - lack of useful candidate positions near the needed corridor
3. design the next heuristic around that later double-fanin starvation rather than around left-edge fanout placement

## 2026-03-15 addendum 8: diagnose the remaining `xor` zero-candidate wall under the batch-winning heuristic

After addendum 7, the retained heuristic became:

- in large-instance native-hex search-depth mode
- reject `fanout` / multi-output candidate positions when `x < 4 && y >= 4`

That was the best batch result so far, but it still did not produce a `w4a4` layout. The next question was whether
the remaining dominant `xor` wall was:

- bottom-row feasibility
- a bad candidate-order choice
- or pure two-input route starvation

### Step 1: add targeted zero-candidate diagnostics for double-fanin native-hex nodes

I added a diagnostic helper in `graph_oriented_layout_design_hex.hpp` that only runs on the rare
`ZERO_CANDIDATES` path for double-fanin nodes. It mirrors the candidate scan and counts:

- total candidate tiles checked
- how many were blocked by the retained left-margin filter
- how many were blocked by the distinct-exit check
- how many died on the first fanin path
- how many died on the second fanin path
- how many died on bottom-row feasibility
- how many would have been accepted

This keeps the normal search behavior unchanged because the extra work only happens when a branch has already failed
with zero candidates.

### Step 2: rerun representative post-filter `xor` seeds

I reran two representative seeds that were failing at the later `xor` wall:

- seed `3`
- seed `80`

Both were run with:

```text
read -a /home/samuelshng/git/verilog-mxu/collab_ext/w4a4_tnano/w4a4_deepsyn3652.v
map --all2 -v
gold -e 3 -s <seed> -g 6 -j -t 60 -c 0 --grid hex
quit
```

Results:

- seed `3`
  - `max placed nodes = 136`
  - deepest failure:
    - `checked=720 accepted=0 left_margin=0 distinct_exit=0 first_path=505 second_path=215 bottom_path=0`
- seed `80`
  - `max placed nodes = 136`
  - deepest failure:
    - `checked=666 accepted=0 left_margin=0 distinct_exit=0 first_path=495 second_path=171 bottom_path=0`

This is a strong result:

- the active `xor` wall is **not** bottom-row feasibility
- the retained left-margin heuristic is **not** what blocks these later `xor` candidates
- the candidates are dying entirely on two-input path construction
- first-path failure is the dominant bucket

### Step 3: test whether simply swapping fanin routing order would help

Because first-path rejection dominated, I extended the same zero-candidate diagnostic to also evaluate the reversed
fanin order on the exact same candidate tiles.

Representative results:

- seed `3`
  - `swapped_accepted=0 swapped_first_path=655 swapped_second_path=65 swapped_bottom_path=0`
- seed `80`
  - `swapped_accepted=0 swapped_first_path=612 swapped_second_path=54 swapped_bottom_path=0`

Interpretation:

- reversing fanin order still yields zero acceptable candidate positions
- the old earlier experiment that tried alternate routing order for all double-fanin gates had already regressed the
  batch
- this narrower diagnostic now shows why a simple order change is unlikely to be the missing fix

So the remaining `xor` wall is **not** a simple “route the other input first” problem.

## Updated takeaway after addendum 8

The retained `x < 4 && y >= 4` left-margin heuristic is still the best batch-level change from this exploration.

But the next wall is now better understood:

- it is later than the old left-edge fanout collapse
- it is mostly `xor`
- it is not bottom-row starvation
- it is not caused by the retained left-margin filter
- it is not fixed by reversing fanin order

That means the remaining failure is more structural:

- the earlier partial placement is reaching states where no candidate tile supports two independent fanin routes for
  the later `xor`

## Best next step after addendum 8

The next promising direction is no longer a local route-order tweak.

The next experiment should target **earlier structural corridor preservation** for later two-input gates, for example:

1. detect whether some early gate/fanout placements are consuming the only later double-fanin corridors near the
   future `xor` frontier
2. add a lightweight feasibility bias or rejection for placements that over-constrict those future corridors
3. evaluate that immediately on the same fixed 15-seed physical-core batch, not on a single hand-picked seed

## 2026-03-15 addendum 9: one-step future-merge lookahead for later double-fanin successors

After addendum 8, I tried the first direct corridor-preservation heuristic.

### Hypothesis

If the remaining wall is caused by earlier placements destroying all later merge corridors for an unplaced `xor`, then
the search might benefit from pruning a branch immediately when a newly placed node already makes one of its future
double-fanin successors impossible.

### Attempted heuristic

I added a targeted native-hex lookahead that only ran:

- in large-instance search-depth mode
- on the just-placed node
- when that node fed an unplaced double-fanin successor
- and the successor’s other fanin was already placed

For that case, the heuristic scanned whether the successor still had at least one feasible merge candidate. If not, it
rejected the branch early with a `future_double_fanin_merge_blocked` failure.

### Representative results

I tested the heuristic on three representative seeds:

- seed `3`, `60s`
- seed `80`, `60s`
- seed `81`, `120s`

Results:

- seed `3`
  - baseline with retained diagnostics: `max placed nodes = 136`
  - with lookahead: `max placed nodes = 126`
  - deepest failure became:
    - `future_double_fanin_merge_blocked succ=xor at=(5, 103) other=(23, 94)`
- seed `80`
  - baseline with retained diagnostics: `max placed nodes = 136`
  - with lookahead: `max placed nodes = 115`
- seed `81`
  - baseline stronger-seed trajectory was already higher than this region
  - with lookahead: `max placed nodes = 163`

Interpretation:

- the heuristic does detect a real structural problem earlier
- but it does not improve search depth on the representative probes
- and on two of the three seeds it clearly makes the search worse

So the lookahead is too blunt in this form. It turns one late wall into an earlier prune, but it does not steer the
search toward better alternatives strongly enough to justify the added rejection power.

I reverted this heuristic and kept only the diagnostic improvements from addendum 8.

## Updated takeaway after addendum 9

The current best retained state is still:

- the left-margin `x < 4 && y >= 4` fanout / multi-output filter
- the zero-candidate `xor` diagnostics, including swapped-order counts

The failed future-merge lookahead shows that the remaining problem is not solved by a simple binary feasibility prune
on the just-placed node. The next successful direction likely needs to be softer than “reject immediately” or more
global than one-step local lookahead.

## Best next step after addendum 9

The next likely productive move is a **soft corridor-preservation bias** instead of a hard prune. For example:

1. score candidate placements for nodes that feed future double-fanin gates by how many downstream merge candidates
   remain, instead of rejecting them outright
2. apply that only in large-instance native-hex mode
3. evaluate it on the same fixed 15-seed physical-core batch
