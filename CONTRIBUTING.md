# Contributing to Crash

Thanks for your interest in Crash — a fast, stable, native Windows 11 file
manager. Contributions are welcome.

## Open-core model

Crash is **open core** (design doc §2.3). The core engine — renderer, enumeration
pipeline, navigation, shell integration, chrome — is MIT-licensed and lives in
this repository. Pro-tier features (currently the command palette; later scripting
and advanced/indexed search) are gated behind a license check (`License.*`). In a
production build the Pro modules would ship separately; here they are present with
a demo unlock so the flow is exercisable.

## Building

Requires **Visual Studio 2022** with the *Desktop development with C++* workload
(MSVC + Windows 10/11 SDK). Everything else (CMake, Ninja) ships with VS.

```powershell
./build.ps1        # -> build/crash.exe
```

or with CMake directly (from a *Developer* prompt):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Architecture

`README.md` has the phase-by-phase overview; each `src/*.h` documents its role.
The load-bearing rule (design §5): **the UI thread never calls a blocking
filesystem or shell API** — enumeration, thumbnails, and search run on worker
threads and stream results back via `PostMessage`. Keep it that way.

## Pull requests

- One focused change per PR; match the surrounding code style (MSVC C++20,
  `/W4 /permissive-`, no new warnings).
- If your change touches product behavior, verify it by **running the app**, not
  just building — the project convention is to confirm features end-to-end.
- Performance is a feature: don't regress the §7 targets (steady refresh-rate
  scroll, no UI-thread stalls). Prefer the existing virtualization / caching
  patterns over per-item objects.

## Reporting bugs

Include your Windows version, whether the issue involves network/cloud/removable
drives (a common source of shell stalls), and steps to reproduce.
