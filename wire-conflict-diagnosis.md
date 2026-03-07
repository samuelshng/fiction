# Wire Conflict Diagnosis

## Scope

This document diagnoses the illegal shared-tile situations observed in:

- `/home/samuelshng/git/fiction-sam/2i2o-exp-out/20260306-201012-ortho_hex_rca2_smoke`

The focus is the native hexagonal `orthogonal_hex` result, especially the half-adder and projected crossover region shown in the screenshot.

No code changes were made as part of this diagnosis.

## Executive Summary

The issue is real.

The failing layout does **not** contain duplicate occupancy of the same exact 3D coordinate `(x, y, z)`, but it **does** contain multiple signals sharing the same projected hex tile `(x, y)` in ways that are not legal under the intended pointy-top port model.

More specifically:

- Some projected double-occupancies are true second-layer uses of the same `(x, y)` tile.
- Several of those are **not** valid crossings.
- At least one half-adder drives both outputs into the same projected successor tile, which makes the visible routing behave as if two outputs leave through the same lower side.
- The current DOT/SVG visualization marks any stacked ground-layer wire plus upper-layer wire as a crossover, even when the stacked pair is not a genuine legal crossing.

Under the intended rule set:

- a pointy-top 2-input tile should consume at most one signal from `NW` and at most one from `NE`
- a 2-output tile should drive at most one signal to `SW` and at most one to `SE`
- same projected tile occupancy should only be legal for explicitly allowed structures such as:
  - crossing: `NE -> SW` together with `NW -> SE`
  - hourglass: `NE -> SE` together with `NW -> SW`
  - possibly a legal fanout variant, if defined to use a shared incoming side and distinct outgoing sides

The diagnosed layout violates that model.

## What Was Inspected

Artifacts:

- `/home/samuelshng/git/fiction-sam/2i2o-exp-out/20260306-201012-ortho_hex_rca2_smoke/20260306-201012-ortho_hex_rca2_smoke.log`
- `/home/samuelshng/git/fiction-sam/2i2o-exp-out/20260306-201012-ortho_hex_rca2_smoke/20260306-201012-ortho_hex_rca2_smoke_ortho_hex_even_row.fgl`
- `/home/samuelshng/git/fiction-sam/2i2o-exp-out/20260306-201012-ortho_hex_rca2_smoke/20260306-201012-ortho_hex_rca2_smoke_ortho_hex_even_row.dot`

Relevant implementation files:

- `/home/samuelshng/git/fiction-sam/include/fiction/algorithms/physical_design/orthogonal_hex.hpp`
- `/home/samuelshng/git/fiction-sam/include/fiction/io/dot_drawers.hpp`
- `/home/samuelshng/git/fiction-sam/include/fiction/io/write_fgl_layout.hpp`
- `/home/samuelshng/git/fiction-sam/include/fiction/layouts/gate_level_layout.hpp`
- `/home/samuelshng/git/fiction-sam/include/fiction/algorithms/verification/design_rule_violations.hpp`

## Confirmed Facts

### 1. There is no exact `(x, y, z)` overlap

The exported FGL does not contain any duplicate exact 3D coordinate.

Therefore, this is **not** a bug where two nodes are assigned to the same exact physical layer location.

### 2. There are many shared projected `(x, y)` tiles

The layout contains multiple positions where both:

- `(x, y, 0)` is occupied, and
- `(x, y, 1)` is occupied

This means multiple routes share the same projected hex tile.

That is only acceptable if the stacked structure is one of the allowed same-tile patterns.

### 3. Some of those projected double-occupancies are illegal

Several stacked wire pairs are not genuine legal crossings or hourglasses. They are simply two wires using the same projected tile with incompatible side usage.

## Concrete Failure Example: First Half Adder

The first half adder is:

- `HA` at `(3, 5, 0)`

In the FGL:

- half adder location:
  - `id 55` at `(3, 5, 0)`
- first routed output branch:
  - `id 56` at `(3, 6, 0)`, incoming from `(3, 5, 0)` output `p=0`
- second routed output branch:
  - `id 70` at `(3, 6, 1)`, incoming from `(3, 5, 0)` output `p=1`

So both HA outputs immediately enter the same projected tile:

- one via `(3, 6, 0)`
- one via `(3, 6, 1)`

That is exactly the situation seen in the screenshot: the HA appears to emit two outputs through the same visible lower side.

This is not a legal crossing:

- both branches come from the same source tile
- both branches first enter the same projected successor
- there is no complementary opposing route pair at that tile

This is also not a legal hourglass.

It is simply an illegal shared-tile launch.

## Concrete Failure Example: Second Half Adder

The same pattern appears again at the second half adder:

- `HA` at `(4, 10, 0)`

Its two outgoing routes immediately occupy:

- `(5, 11, 0)` from output `p=0`
- `(5, 11, 1)` from output `p=1`

Again, both outputs leave through the same projected successor tile.

## Why the Visualization Looks Worse Than the Actual Topology

The rendering currently collapses all layers onto the ground-tile projection.

The DOT drawer labels a ground tile as `"+"` whenever:

- the ground tile hosts a `BUF`, and
- the tile above also hosts a `BUF`

That means:

- true crossings are shown as `+`
- but also any arbitrary stacked wire pair is shown as `+`

Therefore the image over-reports “crossover tiles”.

Some of the suspicious `+` tiles in the screenshot are not legal crossings at all. They are merely stacked wires rendered as if they were a crossover primitive.

## Why the Existing Verification Still Passes

The run log reports:

- all occupied tiles are properly connected
- all wire crossings cross over other wires only

Those checks pass because the current verification is too weak for the rule set you expect.

### Current crossing check

The implemented crossing verification only checks:

- if a wire exists on a crossing layer, then the tile below it must also be a wire

It does **not** check:

- whether the two layers use complementary legal directions
- whether a shared projected tile respects one-port-per-side constraints
- whether the structure is a valid crossing, hourglass, or legal fanout

### Current occupancy / connectivity check

The current connectivity checks ensure that occupied nodes are connected, but they do not encode a per-side capacity model for pointy-top hex tiles.

As a result, a projected tile can be treated as “valid” even if it violates:

- one input per top edge
- one output per bottom edge

## Routing Behavior That Causes the Problem

The root cause is in the routing model used by `orthogonal_hex`.

### 1. Routing works with tile occupancy, not side occupancy

The router reasons primarily about whether a successor tile:

- is empty
- can be reused
- can be crossed by placing a wire above it

It does **not** explicitly track which individual pointy-top sides are already consumed:

- `NW`
- `NE`
- `SW`
- `SE`

### 2. A second route may be materialized by stacking above an occupied successor

When a routed path reaches an occupied projected tile, the path materialization logic may place the new buffer at:

- `above(successor)`

This is allowed whenever the path planner concluded that the occupied successor was routable.

That makes the abstraction:

- “projected tile occupied but still usable via z=1”

However, without a side-capacity model, that same mechanism also allows illegal structures, including:

- two outputs leaving a tile through the same visible side
- stacked same-direction continuations that are not real crossings

### 3. Multi-output gate routing is not constrained by visible output side usage

The half-adder outputs are distinguished by output index `p=0` and `p=1`, but the router does not enforce:

- output 0 must use one bottom side
- output 1 must use the other bottom side

Instead, both outputs may be routed into the same projected successor tile if one of them can be placed on `z=1`.

That is the direct reason the HA appears to have two outputs on one side.

## Fanout Classification

In this codebase, a fanout is a subtype of wire:

- `is_wire(n)` is equivalent to `is_buf(n)`
- `is_fanout(n)` is defined as a wire with multiple outputs

So fanouts are part of the same wire occupancy model.

This matters because legal same-tile sharing must also distinguish between:

- plain wire
- crossing
- hourglass
- fanout

and must validate each using side-specific constraints.

## Interpreting the Failure Against the Intended Rule Set

Given the intended rule set for pointy-top hex tiles:

- inputs must come from top sides only
- outputs must leave from bottom sides only
- each side may carry at most one connection

the failing layout is illegal in at least these ways:

1. Two HA outputs are launched into the same projected successor tile.
2. Several projected stacked wire pairs are not valid crossings.
3. The visualization labels arbitrary stacked wires as crossovers.
4. The current legality checks do not detect side conflicts.

## Most Plausible Fix Directions

These are proposed solutions only. No changes were implemented.

### 1. Add explicit per-side port-capacity tracking

For every occupied pointy-top tile, track usage of:

- `NW` input
- `NE` input
- `SW` output
- `SE` output

Then reject any route that would consume an already used side unless it matches one of the explicitly legal same-tile structures.

This is the most important missing abstraction.

### 2. Encode legal shared-tile patterns explicitly

Instead of treating any ground-wire plus upper-wire pair as a crossing, classify stacked occupancy into allowed forms only:

- crossing: `NE -> SW` and `NW -> SE`
- hourglass: `NE -> SE` and `NW -> SW`
- legal fanout form, if the architecture allows one

Anything else should be rejected.

### 3. Constrain multi-output gates to distinct visible output sides

For half adders and other 2-output gates, bind the two outputs to distinct bottom ports:

- one to `SW`
- one to `SE`

Do not allow both outputs to enter the same projected successor tile, even if one could be placed on `z=1`.

### 4. Strengthen design-rule verification

Add a verification pass that checks, per projected tile:

- top-side input multiplicity
- bottom-side output multiplicity
- legal stacked-pattern classification

This should be stricter than the current “wire above wire” crossing check.

### 5. Fix the visualization

The drawer should not label a tile as `+` merely because there is a wire above a wire.

It should display `+` only if the stacked structure is a true legal crossing according to port directions.

That would make visual debugging substantially more reliable.

## Bottom Line

The observed issue is not an exact same-layer overlap bug.

It is a more specific routing-model bug:

- the current native hexagonal orthogonal flow allows multiple signals to share the same projected hex tile without enforcing the intended one-connection-per-side rule
- and the visualization further masks the difference between legal and illegal stacked uses

So if the intended semantics are:

- only legal crossings, hourglasses, and possibly legal fanouts may share a projected tile

then the current `orthogonal_hex` result is indeed illegal.
