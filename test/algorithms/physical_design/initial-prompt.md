## High level introduction

This repository contains `fiction`, a framework that supports technology mapping as well as placement and routing of field-coupled nanocomputing logic networks, among many other features.

The ultimate goal for me right now is to update the project so that the gold physical design algorithm (see [graph_oriented_layout_design.hpp](include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp)) can support placing and routing 2-input-2-output gate types, rather than being restricted to 1-output gates.

## Pointers from Marcel (primary maintainer of fiction)

The pointers I was given for this effort, by Marcel the primary maintainer of this repo (named fiction), are given below:

- A good starting point for everything that follows would be the multi-output-network branch (`upstream/multi-output-network`), specifically the new [netlist data structure](include/fiction/networks/netlist.hpp) that extends [mockturtle::block_network](https://mockturtle.readthedocs.io/en/latest/implementations.html#block-network). To understand what is going on, it is important to understand how [mockturtle network types](https://mockturtle.readthedocs.io/en/latest/network.html) work, specifically the difference between a node and a signal.
- The goal is to pass fiction::netlist objects (obtained from [technology mapping](include/fiction/algorithms/network_transformation/technology_mapping.hpp)) to our physical design algorithms, which are templatized to accept any network type that follows the mockturtle API. The prime candidates here are [gold](include/fiction/algorithms/physical_design/graph_oriented_layout_design.hpp) and [ortho](include/fiction/algorithms/physical_design/orthogonal.hpp).
- This is where it gets really interesting, but also dicey. Physical design algorithms produce gate-level layouts. A design decision that I regret to this day is that in fiction, gate-level layouts also follow mockturtle's network API, meaning a layout is a network, because it implements the same functions (and then some). That was dumb and needs rewriting desperately! Anyway. Anything that implements the requirements the network API and the additional layout functions can be treated as a layout. This is our default [gate-level layout](include/fiction/layouts/gate_level_layout.hpp). You will see that it requires a bunch of hacks so that it could follow the network API. It's literally held together by faith and duct tape. So this is what you will need to replace. We have a starting point in the multi-output-network branch (`upstream/multi-output-network). It doesn't fully work, though. It could be more efficient to throw this away and start over. The [tests](test/layouts/gate_level_layout.cpp) should give you a good idea of how it must work, and how it is used to [call physical design algorithms](test/algorithms/physical_design/graph_oriented_layout_design.cpp).

## Our work

Essentially, the end goal that I want to achieve, is to be able to do the following in fiction CLI:

```
fiction> read -a path-to-gate-level-verilog.v
fiction> ps -n
fiction> map --all2
fiction> gold -e 3 -m -s $seed -g $skip_tiles -j -t $timeout_per_run -c $cost_func
fiction> ps -g
fiction> hex -io
fiction> ps -g
```

At the end I will have to write the placed and routed layout (_not_ `sqd`) to a file, and I need to be able to do that on either the `gold` output (Cartesian space) or the `hex` output (hexagonal space). I don't remember the exact command name and flags that entails, but keep that requirement in mind.

The technology mapping should take 2-in-2-out gates into account (e.g., be able to use half adder truth table & produce its tile in [truth_table_utils.hpp](include/fiction/utils/truth_table_utils.hpp) under `create_half_adder_tt`) and `gold` should be made able to place and route them.

Note that the `$` variables in the `gold` invocation, such as `$seed`, are just placeholders illustrating parameters that need to continue to work after this refactor, in actual run I'll just be putting in actual supported values for them.

Whereas Marcel had trouble incorporating this because they were trying to cleanly revamp the project's data structure to accommodate this huge shift in requirements, I am currently only seeking to do a proof-of-concept prototype that just needs to work for the specified flow above: read network, technology mapping with 2I2O support, placement and routing with `gold`. In Marcel's pointers, he mentioned that physical design algs that we can consider are `gold` and `ortho`, IMO we can prioritize `gold` unless upon your investigation `ortho` might be easier to implement this with. We have complete freedom to drastically change the data structure such that the resulting pipeline only works specifically for our necessary work here, we can freely disable modules and unit tests that break as a result of our work _if_ those mobules and tests are not relevant to the stated goal.

We also will not need to accommodate `pyfiction` Python header generation _at all_, we only need the CLI to work.

I would like you to carefully inspect all URLs and files mentioned here, and also trace any relevant includes/links to make sure you fully comprehend the existing project. Also investigate `upstream/multi-output-network` and investigate Marcel's WIP effort in making this work. He did state that "it could be more efficient to throw this away and start over", which we need to decide whether we're better off starting over or still merging that branch and building on top. After the careful investigation, create a comprehensive action plan and write to `multi-output-action-plan.md` proposing how we can achieve the stated goal. If there are multiple candidates/possibilities to achieve some/all parts, spell them all out and list their pros and cons.

Extra requests:

- If any URL doesn't work please halt and let me know and I'll make them available to you through other means, but you must not stay silent if a provided URL doesn't work.
