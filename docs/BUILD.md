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

## The triplet is load-bearing

`VCPKG_TARGET_TRIPLET` must be **`x64-windows-static-md`**. It is set in
`CMakePresets.json`, and CI configures through that preset rather than passing
flags by hand.

Without it vcpkg defaults to `x64-windows`, which links spdlog and fmt
*dynamically*. The plugin then builds cleanly, packages cleanly, installs
cleanly — and the game rejects it:

```
checking plugin ...\CSQualityGovernorVR.dll
couldn't load plugin ...\CSQualityGovernorVR.dll (Error 126)
```

Error 126 is `ERROR_MOD_NOT_FOUND`: the plugin was found, its dependencies were
not. Nothing in the build reports a problem, and no plugin log is written at
all, so the first symptom is a wasted game session. That happened on
2026-08-03.

`tools/Check-PluginImports.ps1` now runs in CI after the build and fails if the
DLL imports anything that will not exist at runtime. Diagnosing this from a
built artifact takes seconds; diagnosing it from the game takes a session.
