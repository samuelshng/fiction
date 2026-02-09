# Tech Stack

This document outlines the core technologies, tools, and languages utilized in the `fiction` project.

## 1. Primary Languages

- **C++17:** The primary language for the core library and high-performance algorithms. Adheres to modern C++17 standards for efficiency, safety, and maintainability.
- **Python 3.10+:** Used for scripting, automation, experimental setups, and providing user-friendly bindings to the C++ core. Specific versions 3.10, 3.11, 3.12, 3.13, and 3.14 are supported.

## 2. Build System

- **CMake (3.23+):** Employed for configuring, building, and managing the C++ components of the project. Ensures cross-platform compatibility and efficient build processes.
- **scikit-build-core:** Utilized for building the Python bindings (`mnt.pyfiction`), providing a robust interface between the CMake-based C++ project and the Python ecosystem.

## 3. Key Libraries and Frameworks

### C++ Ecosystem

- **`mockturtle`:** A powerful logic network library used for representing and manipulating various types of logic networks.
- **`kitty`:** A library for truth table manipulation, essential for logic synthesis and verification tasks.
- **`alice`:** A versatile CLI framework that underpins the `fiction` command-line interface, offering a structured approach to tool development.
- **`nlohmann_json`:** A modern C++ JSON library for easy parsing and serialization of JSON data.
- **`fmt`:** A formatting library providing a safe and fast alternative to `printf`/`iostream`.
- **`Catch2`:** A feature-rich C++ test framework used for comprehensive unit and integration testing of the C++ codebase.

### Python Ecosystem

- **`pybind11`:** The chosen library for creating seamless and efficient Python bindings from the C++ core, enabling Python developers to leverage `fiction`'s capabilities.
- **`pytest`:** The standard testing framework for Python components, facilitating the writing and execution of robust tests.
- **`nox`:** Used for managing isolated test environments and automating development tasks, ensuring consistent and reproducible builds.

## 4. Design Automation Tools

- **ABC:** A foundational system for sequential logic synthesis and verification, integrated for advanced logic optimization.
- **Z3 (SMT Solver):** An optional but powerful Satisfiability Modulo Theories solver used for formal verification and certain optimization problems.
- **ALGLIB (Optimization):** An optional numerical analysis and optimization library used for various computational tasks within the framework.

## 5. Field-coupled Nanocomputing (FCN) Technologies Supported

The framework is designed to be technology-independent, but provides specific implementations and simulation capabilities for:

- **Quantum-dot Cellular Automata (QCA):** A prominent FCN technology.
- **in-plane Nanomagnet Logic (iNML):** Another key FCN technology supported by the framework.
- **Silicon Dangling Bonds (SiDBs):** A cutting-edge FCN technology with specialized simulation algorithms.

## 6. Continuous Integration and Deployment (CI/CD)

- **GitHub Actions:** The chosen platform for automating build, test, and deployment workflows, ensuring code quality and rapid iteration. Includes workflows for Ubuntu, macOS, Windows, Python Bindings, Docker Image, and CodeQL analysis.

## 7. Documentation

- **Doxygen:** Used for generating API documentation for the C++ codebase.
- **Sphinx:** Utilized for creating comprehensive project documentation, including user guides, tutorials, and Python API references, often integrating Doxygen output.
