# VQE — Vectorized Analytical Query Engine

A C++20 engine for constructed analytical query plans.

See [implementation milestones](docs/milestones.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
