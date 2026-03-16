# Hex Optimize Exploration

## Goal

Improve native post-layout optimization for row-clocked hexagonal layouts, with the immediate benchmark target
`benchmarks/TOY/RCA2.v` mapped through `orthogonal_hex --hex even_row`.

Current stubborn case as of 2026-03-07:

- Before optimization: `14 x 29`
- After optimization: `8 x 29`
- Desired direction: reduce `y`, not just `x`

## Baseline Observations

- The original native hex optimizer mainly removed unused blank columns by bounding-box compaction and final reroute.
- Several layouts, including the RCA2 smoke case, still contained tall regions whose rows were dominated by routing.
- The cartesian optimizer is materially stronger because it uses `obstruction_layout`, local rip-up and reroute with
  rollback, repeated single-gate relocation passes, and `wiring_reduction`.
- Native hex does not have a comparable incremental obstruction-aware reroute layer.

## Implemented Attempts

### 1. Native hex reroute correctness fixes

Changes:

- Preserve source output pins and target input slots in extracted reroute objectives.
- Treat fanout buffers as recreatable routing instead of fixed structure.
- Reinsert fanins in the correct order when rebuilding targets.

Outcome:

- Fixed a real correctness bug.
- Removed the old warning about discarding a non-equivalent native hex rewiring candidate.
- Did not improve RCA2 height.

### 2. PO border compaction

Changes:

- Added a pass that tries to move POs to a smaller common bottom border row.
- Later corrected this pass to move existing PO nodes instead of recreating them incorrectly.

Outcome:

- Works on some layouts.
- For RCA2, every shorter feasible PO row tested remained unroutable with the current fixed structural geometry.

### 3. Wire-row compaction

Changes:

- Added removal of fully removable wire-only rows with reroute and equivalence checking afterward.

Outcome:

- Safe and occasionally useful.
- RCA2 still stayed at `y = 29`, indicating the remaining blocker is not just obvious empty or wire-only rows.

### 4. First structural gate relocation for native hex

Changes:

- Added a conservative single-gate move pass.
- Initial version moved one gate and then rebuilt global routing.

Outcome:

- Too weak compared with cartesian relocation.
- Candidate moves were often rejected because the entire layout had to reroute immediately.

### 5. Local neighborhood relocation

Changes:

- Extract local fanin/fanout neighborhood for one moved structural gate.
- Clear only recreatable local wires.
- Permit moving into tiles occupied by that gate's own removable wires.
- Rebuild only the local neighborhood instead of always rerouting everything first.

Outcome:

- More faithful to cartesian local relocation.
- Still did not unlock RCA2 vertical shrinkage.

### 6. Crossing policy alignment with cartesian mode

Changes:

- Native hex rerouting now respects `planar_optimization` instead of hardcoding `crossings = false`.

Outcome:

- Important for consistency and for layouts already using crossings.
- No RCA2 height improvement.

### 7. Structural compactness acceptance

Changes:

- Added a coarse structural-gate score based on summed `(y, x)` positions.
- Single-gate relocation can now accept enabling moves that preserve cost bounds but improve top-left compactness.

Outcome:

- More permissive and closer in spirit to repeated cartesian relocation.
- RCA2 still remains `8 x 29`.

### 8. Structural suffix shift compaction

Changes:

- Added a pass that tries to shift every structural node below a chosen cut row upward by one row together, then reroute.
- This is a conservative multi-gate move, intended to go beyond pure single-gate relocation.

Outcome:

- Pass is safe under current regression coverage.
- RCA2 still remains `8 x 29`.

### 9. Bounded uphill exploration with final compare-back

Changes:

- Allowed some intermediate relocation and suffix-shift candidates that slightly worsen wiring while improving
  structural compactness.
- Added a final safeguard that keeps the optimized layout only if it is still equivalent to the original input and
  improves it under the same wire-first policy.

Outcome:

- Safe under the current regression suite.
- Did not change RCA2.

### 10. Full global reroute fallback after moved geometry

Changes:

- If local gate-neighborhood rerouting fails, try a fallback that updates the moved gate geometry and reroutes the
  whole layout from scratch.
- Also refine accepted native-hex candidates with an additional global reroute pass afterward.

Outcome:

- This closes another gap to cartesian behavior, where relocation is stronger than a naive local reroute.
- RCA2 still remains `8 x 29`.

### 11. Bounded two-step relocation exploration

Changes:

- Added a small beam of promising first-step relocation candidates.
- For each seed candidate, reran the greedy native-hex gate relocation, suffix compaction, PO compaction, and global
  reroute refinement.

Outcome:

- This is the first implemented attempt that can model short relocation sequences instead of only one-step wins.
- RCA2 still remains `8 x 29`.

### 12. GOLD-inspired rebuild fallback from native hex layouts

Changes:

- Tried to reconstruct a routing-free structural network from the routed native hex layout.
- Planned to feed that reduced network into `graph_oriented_layout_design_hex` as a bounded fallback for small,
  obviously overstretched layouts.

Outcome:

- This did not become usable yet.
- The rebuild is blocked by interface reconstruction, not by GOLD itself: some PO drivers cannot currently be
  recovered robustly from the routed native layout through the available public APIs, so the extracted network is not
  yet trustworthy enough to hand to GOLD automatically.
- The experimental fallback was therefore left disabled in the main optimizer rather than shipping unstable behavior.

### 13. Structural extraction fixes for native hex GOLD fallback

Changes:

- Added a dedicated regression that extracts a structural network from an orthogonal native-hex RCA2 layout and checks
  equivalence against both the mapped network and the original benchmark.
- Fixed three concrete extraction bugs:
  - PO nodes were accidentally treated as structural gates during `foreach_gate` reconstruction.
  - PI mappings were seeded with `get_node(pi)` even though `foreach_pi` yields PI nodes directly.
  - PO source recovery now falls back to the extracted routing objective for POs that expose no traversable fanin.
- Tightened extraction failure handling so hidden interface mismatches fail explicitly instead of silently inventing
  extra PIs.

Outcome:

- Structural extraction is now correct for orthogonal native-hex RCA2.
- The original "cannot reconstruct PO/source information" blocker is resolved.

### 14. GOLD fallback revival with bounded area-vs-wire acceptance

Changes:

- Re-enabled the bounded native-hex GOLD fallback after extraction became reliable.
- Normalized the extracted network with `mockturtle::cleanup_dangling` before handing it to GOLD.
- Aligned the fallback GOLD parameter set with the existing stable mapped-RCA2 native-hex GOLD test configuration:
  `MAXIMUM_EFFORT`, `return_first = true`, multithreading enabled, `seed = 0`, and `tiles_to_skip_between_pis = 1`.
- Added a dedicated regression proving the fallback is safe and equivalent on orthogonal RCA2.
- Switched final candidate selection from a strict "never increase wires" rule to a bounded area-vs-wire tradeoff
  (`area + 16 * internal_wires`) so strongly more compact layouts are no longer rejected solely for a very small wire
  increase.

Outcome:

- This is the first attempt that materially improves the RCA2 target.
- RCA2 now improves from `14 x 29` to `9 x 20`.
- The accepted RCA2 result is equivalent and DRV-clean, but it trades `111 -> 113` internal wires and `11 -> 22`
  crossings for the much smaller footprint.

## Failed or Discarded Exploration

### Synthetic structural-gap regression

Attempt:

- Build an artificial degraded full-adder hex layout with an inserted structural gap and assert native hex optimization
  removes it.

Outcome:

- The synthetic degrader itself was not robust enough; it produced cases with equivalence instability and therefore was
  removed instead of being kept as a misleading regression.

## Working Hypotheses

### H1. Native hex still lacks obstruction-aware incremental rerouting

The biggest remaining difference from cartesian optimize is not candidate enumeration but reroute sophistication.
Cartesian relocation succeeds because it can locally rip up, test, and roll back paths under obstructions.
Native hex still largely relies on whole-layout or neighborhood A\* rebuilds on the concrete layout.

### H2. Candidate screening in hex is still too geometry-agnostic

GOLD hex already filters placements based on whether important border reachability remains possible under current
obstructions. Native hex relocation currently does not use analogous routability screening before expensive reroute
attempts.

### H3. Native hex needed a global restart plus a softer final selector

The main missing ingredient for RCA2 was not another local move heuristic, but a safe way to invoke the stronger hex
GOLD placer on the extracted structural network and then compare its result with a bounded area-vs-wire tradeoff.

### H4. The remaining optimization gap is now mostly about crossings

The new `9 x 20` RCA2 result shows that height reduction is possible, but the accepted compact solution pays for that
with more crossings. The next quality improvement is therefore not "find any shorter solution" but "preserve as much of
that height gain while reducing crossings and excess wire overhead".

## Next Steps

1. Add a crossing-aware tie-breaker on top of the new area-vs-wire tradeoff so the optimizer can prefer compact GOLD
   fallback layouts with fewer crossings when multiple short solutions exist.
2. Use the accepted GOLD candidate as a seed for an additional local crossing-reduction pass instead of comparing the
   raw fallback result directly to the local native-hex solution.
3. Revisit obstruction-aware incremental rerouting for native hex relocation so local search can recover some of the
   `9 x 20` compactness without paying as much in crossings.
