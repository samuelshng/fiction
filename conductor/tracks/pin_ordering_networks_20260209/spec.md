# Specification: Implement Pin Ordering Networks to Unscramble Input/Output Pins in a Layout

## 1. Introduction

This document specifies the requirements for implementing "Pin Ordering Networks" within the `fiction` framework. The primary goal is to provide a robust and efficient mechanism to reorder the input and output pins of Field-coupled Nanocomputing (FCN) layouts, enabling flexible integration with external tools and design constraints.

## 2. Problem Statement

FCN layouts, especially after physical design algorithms, often have input and output pins arranged in a fixed or sub-optimal order. This can pose challenges for interfacing with other design stages, verification tools, or physical implementations that require specific pin assignments. Currently, there is no dedicated, automated method within `fiction` to systematically unscramble or reorder these pins.

## 3. Goals

The main goals of this feature are:

- To enable designers to specify a desired ordering for input and output pins in an FCN layout.
- To provide an algorithmic solution (Pin Ordering Networks) that can transform an existing layout to match the specified pin order while maintaining functional correctness.
- To integrate this functionality seamlessly with `fiction`'s existing layout data structures and algorithms.
- To ensure the implemented solution is efficient and scalable for practical FCN designs.

## 4. Requirements

### 4.1. Functional Requirements

- **FR1: Pin Order Specification:** The system shall allow users to specify a desired mapping from current pin positions/indices to new pin positions/indices for both inputs and outputs.
- **FR2: Layout Transformation:** The system shall implement an algorithm (Pin Ordering Network) capable of reconfiguring the internal wiring and logic of an FCN layout to achieve the specified pin ordering.
- **FR3: Functional Correctness Preservation:** The transformed layout shall be functionally equivalent to the original layout, i.e., it must compute the same Boolean function.
- **FR4: Integration with Existing Layouts:** The solution shall operate on `fiction`'s generic layout types and be compatible with different FCN technologies (QCA, iNML, SiDB) as much as possible, leveraging technology abstraction.
- **FR5: Input/Output Handling:** The solution shall support reordering of both primary input and primary output pins independently or simultaneously.

### 4.2. Non-Functional Requirements

- **NFR1: Performance:** The pin ordering algorithm shall be reasonably efficient, aiming to complete reordering for typical layouts within practical time limits.
- **NFR2: Maintainability:** The code shall adhere to the project's C++17 coding standards, including `snake_case` naming, Doxygen documentation, and `const` correctness. Python bindings shall follow `PascalCase` for classes, `snake_case` for functions/variables, and include type hints and Google-style docstrings.
- **NFR3: Testability:** The implementation shall be thoroughly unit-tested using `Catch2` for C++ and `pytest` for Python bindings, targeting >95% code coverage for new components.
- **NFR4: Error Handling:** The system shall provide clear error messages for invalid pin specifications or unachievable reorderings.
- **NFR5: Documentation:** Comprehensive documentation shall be provided, explaining how to use the Pin Ordering Networks, their limitations, and performance characteristics.

## 5. Scope

The initial scope of this track will focus on:

- Developing the core algorithmic logic for Pin Ordering Networks.
- Integrating this logic with existing generic FCN layout representations.
- Providing a basic interface for specifying pin reordering.
- Ensuring functional correctness through verification.

Out of scope for this initial track:

- Automatic synthesis of optimal pin ordering.
- Complex multi-layer routing solutions specifically for pin reordering beyond the capabilities of existing layout algorithms.

## 6. Open Questions / Dependencies

- What is the preferred method for users to specify pin mappings (e.g., text file format, API call with lists/maps)?
- Are there existing `mockturtle` or `kitty` functionalities that can be leveraged for functional equivalence checking during or after reordering?
- Potential impact on physical area and delay after reordering – how to measure and report this?
- Interaction with clocking schemes during the reordering process.
- How to handle cases where a specified reordering is physically impossible within the given layout constraints.
