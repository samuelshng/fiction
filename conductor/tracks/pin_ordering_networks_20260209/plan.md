# Implementation Plan: Unscrambling Pin Orderings in Gate-Level Layouts

This plan outlines the steps for implementing a physical design algorithm to unscramble input and output pin orderings in `fiction` gate-level layouts. The project will follow Test-Driven Development (TDD) principles.

## Phase 1: Research and Foundational API Work

- [x] **Task: Research Pin Unscrambling and Sorting Networks.**
  - [x] Subtask: Investigate general algorithms for creating permutation networks using wires and crossings (e.g., Beneš networks, butterfly networks).
  - [x] Subtask: Research "sorting networks" (e.g., bitonic, odd-even) as a potential alternative for ordering pins.
  - [x] Subtask: Analyze trade-offs between different network topologies in terms of area, path lengths, and implementation complexity within the constraints of FCN technologies.
  - [x] Subtask: Document findings to inform the algorithm selection and design in a Markdown file. Await approval by the user.

- [x] **Task: Define and Implement the Physical Design Algorithm's Interface.**
  - [x] Subtask: Create a new algorithm file `include/fiction/algorithms/physical_design/unscramble_pins.hpp` following the structure of existing physical design algorithms (e.g., `orthogonal.hpp` or `post_layout_optmization.hpp`).
  - [x] Subtask: Define the main function signature that accepts a `gate_level_layout` as a template paramter `Lyt` and the desired input/output pin orderings (e.g., as `std::vector` of pins).
  - [x] Subtask: Write unit tests to verify the `gate_level_layout` API for iterating over Primary Inputs (PIs) and Primary Outputs (POs) and querying their locations (coordinates). This is to ensure the necessary API is understood and working as expected. This can be inferred from existing `gate_level_layout` tests.

- [ ] **Task: Conductor - User Manual Verification 'Phase 1: Research and Foundational API Work' (Protocol in workflow.md)**

## Phase 2: Core Algorithm Implementation for Hexagonal Layouts

- [x] **Task: Implement Core Helpers for Metric-Based Routing.**
  - [x] Subtask: Implement `determine_pin_coordinates`: Extract layout coordinates for a given vector of pin nodes.
  - [x] Subtask: Implement `calculate_rows_needed`: Calculate the maximum vertical space (rows) required based on horizontal displacement (`dist * 2`).
  - [x] Subtask: Write unit tests for these helper functions to verify distance and row calculations.

- [ ] **Task: Implement the A\* Routing Logic.**
  - [ ] Subtask: Use the calculated rows to determine the routing area.
  - [ ] Subtask: Implement the routing loop using `fiction::a_star` to connect current pin locations to their target slots at the bottom of the routing channel.
  - [ ] Subtask: Ensure strictly disjoint paths where possible, or use the layout's obstruction handling.

- [ ] **Task: Integrate and Verify.**
  - [ ] Subtask: Combine helpers and routing into the main `unscramble_pins` function.
  - [ ] Subtask: Verify with integration tests on small and large layouts.

- [ ] **Task: Conductor - User Manual Verification 'Phase 2: Core Algorithm Implementation for Hexagonal Layouts' (Protocol in workflow.md)**

## Phase 3: Functional Verification and Expansion

- [ ] **Task: Implement Functional Correctness Verification.**
  - [ ] Subtask: Write tests that check if the Boolean function of the layout is preserved after unscrambling.
  - [ ] Subtask: Integrate the existing equivalence checking algorithm `fiction::equivalence_checking` to formally verify the functional equivalence between the original and the unscrambled layout.

- [ ] **Task: Extend support to Cartesian Layouts (Optional Goal).**
  - [ ] Subtask: Adapt the unscrambling logic to work with the connectivity constraints of Cartesian grid layouts and arbitrary clocking schemes, i.e., connectivities.
  - [ ] Subtask: Add tests for Cartesian layouts and other clocking schemes.

- [ ] **Task: Conductor - User Manual Verification 'Phase 3: Functional Verification and Expansion' (Protocol in workflow.md)**

## Phase 4: CLI/Python Bindings and Documentation

- [ ] **Task: Implement CLI commands for the pin unscrambling algorithm.**
  - [ ] Subtask: Design CLI arguments and output formats.
  - [ ] Subtask: Implement the CLI command within the `fiction` CLI application.

- [ ] **Task: Create Python bindings for the unscrambling functionality.**
  - [ ] Subtask: Design a Python API for the algorithm.
  - [ ] Subtask: Write `pytest` tests for the Python bindings.
  - [ ] Subtask: Implement the Python bindings using `pybind11`.

- [ ] **Task: Extend Documentation.**
  - [ ] Subtask: Write Doxygen documentation for the C++ implementation.
  - [ ] Subtask: Add the Python bindings to the documentation.

- [ ] **Task: Conductor - User Manual Verification 'Phase 4: CLI/Python Bindings and Documentation' (Protocol in workflow.md)**
