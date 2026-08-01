# Building

There is no local dev kit on the target machine — all compilation happens in
GitHub CI.

## Two independent halves

| Target | Needs | CI status |
|---|---|---|
| `csgov_core` + tests | CMake and a C++20 compiler. Nothing else. | **gating** |
| `CSQualityGovernorVR.dll` | CommonLibSSE-NG via vcpkg | `continue-on-error` |

The core is deliberately free of SKSE, the Skyrim SDK and Windows, which is why
its tests build and run on both Linux and Windows in about a minute. **That is
the job to trust when changing logic.**

```bash
cmake -S . -B build -DCSGOV_BUILD_TESTS=ON -DCSGOV_BUILD_PLUGIN=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Known issue: the plugin job does not build yet

```
error: while loading commonlibsse-ng:
error: commonlibsse-ng does not exist
vcpkg install failed
```

`commonlibsse-ng` is **not in the mainline vcpkg registry**. It is published
through a custom registry, so `vcpkg.json` alone is not enough — the project
needs a `vcpkg-configuration.json` declaring that registry and a pinned
baseline.

This is unresolved deliberately rather than guessed at: naming the wrong
registry produces a more confusing failure than naming none, and each attempt
costs a CI round trip. Resolve it by taking the registry and baseline from a
known-good CommonLibSSE-NG project template rather than from memory.

Until then the plugin job is `continue-on-error`, so it reports the problem
without failing the run.

## Portability note

The core is compiled by both MSVC and GCC on purpose. MSVC already accepted a
construct GCC rejected — a defaulted `Config a_config = {}` argument on a
constructor whose parameter type is nested in the same incomplete class. Two
compilers catch more than one, and the tests are cheap.
