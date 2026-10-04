# RotWK live skeleton preparation

Date: 2026-10-03. This implements the first runtime offload proposed in
[the subsystem report](rotwk-subsystem-offload-2026-10-03.md). That earlier
commit contained an isolated experiment; this change installs real hooks.

## Runtime behavior

On the verified direct EXE and DAT profiles, raw-animation skeleton workers
are enabled by default when at least three physical cores are allowed.
`AOTR_SKELETONWORKERS=0` disables this capability. Main and render core roles
are reserved; the shared joined preparation pool uses at most two additional
physical cores. Compute threads have actual affinity masks, rather than only
ideal-processor hints. One- and two-core configurations install no skeleton
hooks. Pool startup happens outside the loader lock, when work is first needed.

The scene hook collects recently updated, top-level, manually advanced HLod
objects. Eligible objects must have the verified concrete raw-animation clip,
an independently owned hierarchy of 2–512 pivots, prior-parent links, no
captured bones, no slaving, no auto advance and no blend. A pass needs at least
32 objects and 2,048 bones to dispatch. At most 2,048 jobs and 131,072 private
pivots are retained. Private storage commits pages as needed, reuses hierarchy
inputs, and resets its bounded arena when needed.

Workers run a checked copy of the native per-bone arithmetic into private
pivots. The native x87 root matrix conversion and frame rounding remain on
the main thread. Speculative preparation restores the caller's x87 environment
and MXCSR. At the original tree update, every root, animation, tree and clip
header input, base quaternion/translation, bone index and parent link is
checked again. A changed input executes the original update and refreshes the
private input cache next pass. Publishing copies only fields written by the
native routine. Main-thread callbacks, animation selection, child updates,
bounds, skinning, simulation and D3D submission retain their original paths.

The first 512 prepared results are compared against native updates; those
calls return the native outputs. Afterward, one in 64 is checked. A mismatch
or worker fault disables the capability for the session. Unsupported layouts,
unmasked exceptions, foreign callers, busy pools and allocation failures
retain native updates. No preparation survives beyond its scene call.

Installation validates ten complete native regions, including concrete clip
getters and its vtable. The scene and tree hooks commit transactionally under
the existing verified retail profile gate. The patched EXE is intentionally
supported independently of the DAT's different overall text hash.

## Validation and measured gain

The offline fixture maps a private image of each supplied binary into a test
child; it does not launch or modify the game. It invokes the installed scene
and tree detours, actual native tree dispatch and actual raw clip getters.
Only scene traversal callbacks are controlled by the fixture; no GPU runs.

Both reporting and production builds passed 273,024 native bone comparisons
per image and build. Cases cover several root rotations, fractional animation
frames, single/double/extended x87 precision, four rounding modes, FTZ/DAZ,
complete x87 control/tag/status and MXCSR equality, stale root/frame/base/
scale/parent/clip inputs, proof-to-live transition, deliberate proof mismatch,
foreign scene forwarding, busy-pool fallback and generation wrap. Layout,
modified-code, opt-out and one-/two-core installation rejection also pass.
The existing work fixture passes with the shared pool affinity change,
including color FP behavior, fallback, shutdown/restart and topology checks.

The benchmarks include collection, snapshots, validation, publishing and
dirtying, with persistent inputs warmed. Seven alternating samples report
the median per complete update pass. They use 64 bones per unit and mixed
root rotations on an i7-1165G7 (four physical cores/eight logical CPUs).
Main is pinned to one logical processor; two compute workers use spare
physical cores. The reserved render core is idle in the fixture.

| Image | Units | Native, microseconds | Workers, microseconds | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Patched EXE | 512 | 1,262.588 | 938.440 | 1.345x |
| Patched EXE | 1,500 | 4,054.868 | 3,317.186 | 1.222x |
| DAT | 512 | 1,703.233 | 1,205.331 | 1.413x |
| DAT | 1,500 | 3,859.840 | 3,062.219 | 1.260x |

Full logs: [EXE](benchmarks/rotwk-skeleton-exe-2026-10-03.log),
[DAT](benchmarks/rotwk-skeleton-dat-2026-10-03.log).
The `PROFILE` rows include cold storage allocation and are diagnostic timings,
not the warmed benchmark results. Clock speed and background load affect
absolute times. Integration and copies reduce the earlier experiment's
roughly 2.8x raw-kernel result. In these complete passes, savings are about
0.32–0.80 ms. These are skeleton-stage gains, not measured whole-game FPS.
Active rendering contention, actual qualifying-object counts and late-battle
simulation remain unmeasured. This change does not establish 120 FPS.

For example, if eligible skeleton work accounts for 40% of the frame's CPU
critical path, the measured 1.22–1.41x stage result implies approximately
7–12% shorter CPU frames, before active-render contention. The actual share
must be measured in a battle.

## Reproduction

Set `AOTR_VCVARS32` to the x86 Visual Studio environment script, then:

```powershell
py src\prepare_particle_retail_test.py --retail C:\RotWK\lotrbfme2ep1.exe --skeleton --output "$env:TEMP\rotwk-skeleton-exe.bin"
src\build_rotwk_skeleton_test.bat "$env:TEMP\rotwk-skeleton-exe.bin" benchmark
```

Repeat with `C:\Users\vvval\Downloads\game (1)\game.dat` and a separate
temporary output. Generated native code remains outside the repository.
`src\build_new.bat` and `src\build_prod.bat` build the integrated DLL;
both overwrite `src\bfme2_accel.new.dll`, so run them sequentially.
