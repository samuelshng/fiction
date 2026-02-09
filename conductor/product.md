# Product Definition

## Initial Concept

_fiction_ is an open-source design automation framework specifically tailored for Field-coupled Nanotechnologies (FCN). It provides a comprehensive C++17 header-only library and Python bindings (`mnt.pyfiction`) to implement and evaluate state-of-the-art algorithms across various stages of the FCN design flow.

## Key Features

- **Logic Synthesis:** Integrates with tools like ABC and mockturtle for logic network optimization.
- **Physical Design:** Offers algorithms for automatic placement, routing, and clocking of FCN circuits, supporting both exact and scalable methods.
- **Verification:** Includes Design Rule Violation (DRV) checking and SAT-based formal verification.
- **Physical Simulation:** Provides simulation algorithms for various FCN technologies, including Quantum-dot Cellular Automata (QCA), in-plane Nanomagnet Logic (iNML), and Silicon Dangling Bonds (SiDBs).
- **Technology Abstraction:** Enables technology-independent physical design, with support for specific technology implementations for physical simulation.
- **Clocking Schemes:** Supports various regular and irregular clocking schemes.
- **Python Bindings:** Allows scripting and rapid prototyping through `mnt.pyfiction`.
- **CLI Tool:** Provides a stand-alone command-line interface for quick access to core functionalities.

## Technical Foundation and Development Principles

The `fiction` project is built upon a strong technical foundation and adheres to strict development principles to ensure high performance, safety, readability, and maintainability.

### Core Technologies

- **Primary Languages:** C++17 (with `clang-format` and `clang-tidy` for code quality), Python 3.10+ (with `ruff` for linting/formatting and `mypy` for type checking).
- **Build System:** CMake 3.23+ for C++ and `scikit-build-core` for Python bindings.
- **Python Bindings:** Leverages `pybind11` for seamless integration between C++ and Python.
- **Testing:** `Catch2` for C++ unit tests and `pytest` for Python testing, with `nox` for isolated test environments.
- **CI/CD:** Managed via GitHub Actions for continuous integration and deployment.
- **Documentation:** Generated using Doxygen for C++ and Sphinx for Python, with a strong emphasis on comprehensive and up-to-date documentation.

### Development Principles

- **Architectural Oversight:** Prioritization of overall project architecture, maintainability, and modern best practices across the entire tech stack.
- **Code Quality:** Strict adherence to C++ and Python coding standards, including naming conventions (`snake_case` for C++/Python functions/variables, `PascalCase` for Python classes, `UPPER_SNAKE_CASE` for macros), mandatory type hints in Python, and comprehensive Doxygen documentation for all C++ symbols.
- **Testing Culture:** All new functionality requires accompanying unit tests.
- **Dependency Management:** Strategic use of well-established libraries like `kitty`, `mockturtle`, `alice`, `nlohmann_json`, `fmt`, `Z3`, and `ALGLIB`.
- **Safety and Robustness:** Emphasis on `const` correctness in C++, preference for STL, and use of braced initialization.
