# Dependencies and local setup

The core engine and test harness require only C++20 and CMake. Optional
integrations in this checkout were built with:

- Apache Arrow/Parquet 21.0.0, C++ headers and libraries shipped by the PyArrow
  21.0.0 macOS ARM64 wheel for CPython 3.13. Python is used only to obtain the
  package; the engine links directly to the C++ libraries.
- Google Benchmark 1.9.4, built in Release mode from its upstream source archive.

Both are Apache-2.0 projects; their distributions retain their own license and
notice files under the ignored `.deps/` directory. They are not copied into the
repository's source tree. CMake uses installed packages and never invokes
FetchContent, a package manager, or a network download.

The following are explicit, optional **network download commands** for recreating
the dependency directory on a suitable system. Only run them when downloads are
allowed. `pip --only-binary` fails if a wheel is unavailable for the platform;
use installed Arrow CMake packages instead in that case.

```sh
mkdir -p .deps
python3 -m pip install --only-binary=:all: --no-deps \
  --target .deps/python pyarrow==21.0.0
curl --fail --location --output .deps/benchmark-1.9.4.tar.gz \
  https://codeload.github.com/google/benchmark/tar.gz/refs/tags/v1.9.4
tar -xzf .deps/benchmark-1.9.4.tar.gz -C .deps
cmake -S .deps/benchmark-1.9.4 -B .deps/benchmark-build \
  -DCMAKE_BUILD_TYPE=Release -DBENCHMARK_ENABLE_TESTING=OFF \
  -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/.deps/install"
cmake --build .deps/benchmark-build -j
cmake --install .deps/benchmark-build
```

Core builds do not need any of these steps. Local commit history was
[reconstructed](history-reconstruction.md) on September 17, 2026. The project
has no remote, release, package upload or publication workflow.
