# Project guidance

Build a fast, modular, reusable open-world engine. Runtime performance is a primary design constraint; keep the implementation understandable and practical. Use C++17, Vulkan, SDL3, HLSL, and the existing CMake/CLion workflow.

## Design before implementation

- At the start of a task, read the relevant code, [system design](docs/system_design.md), and [milestones](docs/milestones.md). Choose appropriate abstractions, data structures, and algorithms before implementing system changes.
- Document new or changed system designs in `docs/system_design.md` before implementation, and update the document to match the final result. Explain responsibilities, interfaces, ownership, data layout, and important performance tradeoffs in proportion to the change.
- Give detailed designs their own Markdown files under `docs/` and link them from `docs/system_design.md`. Keep that document a useful overview and index. Clearly distinguish proposed designs from implemented systems.
- Prefer small, useful abstractions with clear boundaries. Generalize around concrete needs; avoid speculative frameworks, unnecessary layers, or exhaustive design documents for trivial fixes. Routine fixes can reference an existing design without creating a new one.

## Performance and memory

- Choose algorithms and data layouts around the actual workload, access patterns, and expected scale. Prefer contiguous storage, locality, batching, and bounded work where appropriate.
- Avoid trips through the system memory allocator during steady-state frame processing. Preallocate and reuse containers, pools, arenas, scratch storage, and upload buffers as appropriate. Watch for hidden allocations in container growth, temporary strings, callbacks, and per-object heap storage.
- Define capacities, lifetimes, and exhaustion behaviour for bounded storage. Handle growth deliberately outside critical frame work where possible; do not silently replace allocation stalls with unbounded memory use.
- Prefer typed handles to long-lived pointers for resources and objects managed by a system. Use generation checks when slots can be recycled, and define invalid-handle behaviour. Short-lived pointers and references are appropriate for direct access when ownership and lifetime are clear; do not add handle lookups inside tight loops unnecessarily.
- Make CPU/GPU ownership, synchronization, and resource retirement explicit. Reuse or release resources only when their users, including submitted GPU work, have finished. Avoid blocking waits in ordinary frame processing where practical.
- Measure meaningful performance changes in representative workloads. Consider frame-time spikes, allocations, memory use, and CPU/GPU costs as well as average throughput. State clearly when an improvement is expected rather than measured.

## Modularity and implementation quality

- Keep application entry points such as `main` focused on startup, shutdown, and orchestration. Put subsystem implementation in cohesive source/header files, and split growing files when distinct responsibilities make them difficult to navigate. Use practical judgment: related types and functions can share a file; avoid rigid one-class-per-file rules or fragmentation that makes the code harder to follow.
- Never let OOP conventions, inheritance hierarchies, design patterns, or abstraction purity dictate a slower implementation. Use simple structs, functions, composition, or classes according to the problem. Introduce indirection or virtual dispatch only when it provides a concrete benefit at an acceptable cost.
- Keep systems cohesive and interfaces small. Separate terrain generation, terrain data, rendering, and persistence responsibilities without building a general framework in advance.
- Keep ownership and lifetimes explicit. Use RAII for resource management where appropriate; performance is not a reason to accept leaks, dangling references, races, or undefined behaviour.
- Prefer straightforward, maintainable code and existing facilities when they meet the performance requirements. Avoid cleverness and custom infrastructure without a demonstrated need.
- Validate changes with focused checks appropriate to the affected behaviour, including Vulkan validation for rendering work when available. Document material limitations and keep design notes current.

## Testing

- Aim for thorough, meaningful coverage of our behaviour and invariants: normal use, boundary cases, failure paths, resource lifetimes, and interactions between our systems. Add regression tests for bugs where they provide lasting value.
- Test our code and our integration with dependencies, not the internal correctness of third-party libraries. Avoid trivial tests, assertions that merely repeat the implementation, and tests added only to increase counts or coverage percentages.
- Keep routine tests fast. For expensive computations, use smaller representative inputs, such as small terrain grids, fewer chunks, or reduced iteration counts. Preserve the conditions that exercise the behaviour under test; a reduced workload must not eliminate the bug or boundary case being checked.
- Keep larger stress tests and performance benchmarks separate from the routine correctness suite. Run them when scale, synchronization, or performance is relevant to the change; small tests do not establish production-scale performance.
- Tests must own their fixtures. Use deterministic generated data or small dedicated fixtures maintained with the tests, and isolated temporary directories for file output. Do not depend on game models, materials, textures, world saves, or other content that may change or disappear.
- Prefer assertions about observable results and invariants over implementation details. Use fixed seeds and justified numerical tolerances where appropriate; do not assume identical floating-point results across GPU devices without a defined guarantee.
- Exercise our GPU integration with Vulkan validation when available. Report unavailable hardware or skipped checks explicitly rather than treating unexecuted tests as passing.
- Record the relevant test strategy in each system design and run the checks appropriate to a change before considering it complete.

## Dependencies

Prefer available system packages over building large dependency repositories locally. When a system package installation is needed, use the graphical privilege wrapper (`pkexec`) for package-manager commands, subject to the execution environment's permissions.
