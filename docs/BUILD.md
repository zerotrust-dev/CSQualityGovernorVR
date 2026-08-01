# Building

No local dev kit on the target machine — all compilation happens in GitHub CI.

## Two independent halves

| Target | Needs | CI job |
|---|---|---|
| `csgov_core` + tests | CMake and a C++20 compiler. Nothing else. | `core-tests`, Linux + Windows |
| `CSQualityGovernorVR.dll` | CommonLibSSE-NG VR via vcpkg | `plugin`, Windows |

The core is deliberately free of SKSE, the Skyrim SDK and Windows, which is why
its tests build and run on both platforms in about a minute. **That is the job
to trust when changing logic.**

```bash
cmake -S . -B build -DCSGOV_BUILD_TESTS=ON -DCSGOV_BUILD_PLUGIN=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Plugin dependencies

The package is **`commonlibsse-ng-vr`**, not `commonlibsse-ng`, and it is not in
the mainline vcpkg registry — it comes from the colorglass registry declared in
`vcpkg-configuration.json`.

The first CI attempt failed with `commonlibsse-ng does not exist` because of
exactly that. The fix was not guesswork: the sibling `HeadDirectedTurning`,
`TiptoeToJumpVR` and `TreadmillLocomotionVR` repos already build green, and this
project now copies their configuration verbatim —

- the same colorglass registry and pinned baselines,
- the same `find_package(CommonLibSSE CONFIG REQUIRED)` and
  `add_commonlibsse_plugin(... COMPATIBILITY VR)`,
- the same manual pinned-vcpkg bootstrap in CI rather than a marketplace action.

**When any of that needs changing, check those repos first.** The vcpkg commit
pinned in the workflow must match the baseline in `vcpkg-configuration.json`.

## Portability note

The core compiles under both MSVC and GCC on purpose. MSVC accepted a construct
GCC rejected — a defaulted `Config a_config = {}` argument whose type is nested
in the same still-incomplete class. Two compilers catch more than one, and the
job costs a minute.
