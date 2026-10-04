# RotWK: larger gains through subsystem preparation

Date: 2026-10-03. Baseline: `3fce23d` on `main`.

## Decision

Prioritize a verified port of batched skeleton/model preparation to the supplied direct EXE. Follow it with particle geometry preparation across many systems. Both can move a substantial part of rendering preparation off the main thread. Continue movement work after these boundaries have been established, or when a battle profile shows movement is the dominant cost.

There is already a whole-skeleton/HLod worker implementation in [aotr_posewarm2.inc](../src/aotr_posewarm2.inc). It is behind the legacy `5ED63115` engine gate. The supplied `lotrbfme2ep1.exe` has `.text` hash `88C193EE`; its retail capability path does **not** install that implementation. The optional retail quaternion-to-matrix cache is a different, serial experiment and remains disabled by default. Several relevant native routines match between the supplied EXE and DAT, making a separately verified port practical. Matching these routines is not enough to enable the entire legacy hook set.

The new [offload experiment](../src/rotwk_offload_test.cpp) tests this direction with native raw-animation tree updates and aggregated particle color preparation. It does not install hooks or change runtime activation. This commit supplies measurements and a concrete implementation experiment; the live offload still needs the integration work below.

## What was measured

The fixture maps a private image prepared from the user's binary into a suspended test child. It never starts the game or modifies either installed binary. Skeleton preparation invokes the original routine at `00563310`, rather than a replacement arithmetic model. Each synthetic unit owns its tree, pivots and root transform. Units share one immutable synthetic raw-animation clip, protected with `PAGE_READONLY`, containing frame-varying translation and quaternion channels. The two virtual clip getters are fixture functions that read immutable fields; real animation-class implementations remain outside this proof.

The particle experiment takes 64 independent systems and processes their combined point ranges through one pool wake/join. The comparison is against the accelerator's **existing serial SIMD color packer**, so it measures additional threading benefit rather than repeating the earlier native-to-SIMD gain. Descriptor prefix preparation is included in batch timing. A 32,768-point threshold applies to the aggregate batch: 64 × 4,096 qualifies even though no individual system qualifies for the current runtime worker threshold. A 64 × 192 batch remains serial.

Each row uses seven alternating serial/batch samples and reports the median time per complete batch. Both paths run the same arithmetic and own separate output buffers. The main thread participates in preparation. A single logical CPU per physical core is allowed; workers are pinned to the third and fourth cores. The second core is reserved for rendering and idle in this fixture. Machine: Intel Core i7-1165G7, four physical cores/eight logical CPUs, x86 MSVC `/O2 /MT`.

Full results: [EXE log](benchmarks/rotwk-offload-exe-2026-10-03.log), [DAT log](benchmarks/rotwk-offload-dat-2026-10-03.log), [CSV](benchmarks/rotwk-offload-2026-10-03.csv). Absolute times vary with clock speed, cache behavior and background load. Compare serial and batch within each row, rather than interpreting changing serial times between core configurations as scaling.

Direct-EXE results (microseconds per complete synthetic batch):

| Physical cores | Workload | Serial | Batch | Speedup |
| --- | --- | ---: | ---: | ---: |
| 3 | 64x192 particle colors vs existing serial SIMD | 16.842 | 15.544 | 1.084x |
| 3 | 64x4096 particle colors vs existing serial SIMD | 487.708 | 219.277 | 2.224x |
| 3 | 32 independent native raw-animation trees/64 bones | 101.708 | 64.043 | 1.588x |
| 3 | 512 independent native raw-animation trees/64 bones | 1739.750 | 885.238 | 1.965x |
| 3 | 1500 independent native raw-animation trees/64 bones | 3742.500 | 1944.204 | 1.925x |
| 4 | 64x192 particle colors vs existing serial SIMD | 17.182 | 15.540 | 1.106x |
| 4 | 64x4096 particle colors vs existing serial SIMD | 526.719 | 195.172 | 2.699x |
| 4 | 32 independent native raw-animation trees/64 bones | 102.698 | 59.860 | 1.716x |
| 4 | 512 independent native raw-animation trees/64 bones | 1750.340 | 608.371 | 2.877x |
| 4 | 1500 independent native raw-animation trees/64 bones | 3742.772 | 1330.982 | 2.812x |

The independently prepared DAT run reproduced the direction: with four physical cores, 512 trees ran 2.785x faster, 1,500 trees ran 2.998x faster, and 64 x 4,096 particle colors ran 2.608x faster. In the EXE run, the 512-tree batch saved 1.142 ms and the 1,500-tree batch saved 2.412 ms. These are useful stage reductions before live collection, validation and commit costs.

One and two allowed physical cores create no preparation workers. Any differences in those rows are serial dispatch/code-layout/cache effects or timing variation. They are not multicore gains. This design cannot satisfy the one-core goal by adding threads; that still requires removing passes, copies, allocations, repeated work and draw calls.

## Frame-level gain

This is a larger target than the script lookup or heap helper because it can cover preparation for hundreds of visible units in a single batch. It still cannot turn a stage speedup into the same overall FPS speedup.

If fraction `p` of the CPU critical path becomes `s` times faster, the optimistic frame speedup is `1 / (1 - p + p / s)`. At a conservative 2.5× preparation speedup:

| Preparation share of the CPU critical path | Frame-time reduction | Potential FPS increase |
| --- | ---: | ---: |
| 20% | 12% | 13.6% |
| 40% | 24% | 31.6% |
| 60% | 36% | 56.3% |

These shares are assumptions, not an EXE battle profile. Snapshotting, validation, extra copies, callbacks and contention will reduce the benefit. If the render worker or GPU already dominates, displayed FPS can barely change. With a 2.5× eligible stage, going from 90 to 120 FPS would require that stage to account for at least 41.7% of the original critical path, before integration overhead. Going from 60 to 120 would require 83.3%. There is no basis here to promise 120 FPS in every late battle.

## Core assignments and boundaries

| Core role | Work |
| --- | --- |
| Main | Ordered simulation, combat, RNG, object lifetime, script actions, movement-result commits, serial callbacks, job collection and final validation; also consumes preparation chunks |
| Second physical core | Existing D3D/render API worker and ordered submission |
| Third physical core | Independent skeleton/model numeric preparation and particle geometry chunks |
| Fourth physical core | More chunks from the same preparation pool, rather than a separate thread for every subsystem |

This is a preparation stage that runs after inputs become stable and joins before outputs are consumed. It does not add one frame of latency. If no spare physical core is allowed, execute the same work serially. SMT siblings should not be counted as extra physical compute slots.

## Model port: the first implementation target

Start with independently owned, manually advanced, non-slaved raw-animation trees. Collect the visible/needed objects at a verified render-pass boundary after simulation has finished modifying them. Freeze the root transform, clip, frame, scale and hierarchy inputs. Keep native auto-advance, animation selection, sound triggers, attachments and arbitrary child virtual calls on the main thread.

Workers should calculate into reusable private pivot outputs. Remap parent links into the private hierarchy and reject unsupported external links or capture state. Retain the native quaternion/translation operation order. At the original update point, compare all relevant inputs and ownership/lifetime state; consume the prepared output only if it is still valid. Otherwise execute the original update. Validate every job during initial proof and sample subsequently. A mismatch disables the capability for that session.

The existing warmer is a useful starting point for collection and validation, but it updates canonical trees early and supports more object types. Its profile checks and object qualifiers must be audited separately for this EXE. Do not copy its historical “420 updates / 2.1 ms” comment into an EXE gain estimate; that describes a different capture.

After tree preparation is proven, add owned plain-mesh child transforms, cached bounds and supported skinning preparation. Each addition needs verified concrete methods and output ownership. Nested containers, slaved trees, arbitrary virtual callbacks, shared mutable animation caches and allocation fallbacks stay serial until their dependencies are resolved. Even when rendering the same pose in shadow and normal passes, reuse needs unchanged-input proof; blindly skipping the second pass can change behavior.

## Particle batching: the second target

Color batching proves that many ordinary-size systems can supply enough work for two compute workers. Its kernel alone is not the desired final offload. Include selected-point gathering, camera-space transforms, billboard vertices, packed colors and UVs in one broader geometry job where the native layout permits it. Reuse output capacity and shared immutable quad indices. This can reduce temporary arrays and passes even on one core.

The current color hook's caller consumes its output immediately. Queueing the hook and returning would publish unfinished colors. First establish a whole-pass preparation boundary where visual inputs are ready, then join before native consumers. Preserve system, particle and transparency order, texture/shader state and capacity flushes.

PointGroup's current compressed position/color/size/orientation/frame arrays are shared global scratch. Running stock PointGroup functions concurrently would race. Each worker needs private outputs and a verified handoff. Open-BFME-2's `pointgr.cpp` has stubbed, present-unmatched render bodies, so source declarations alone cannot establish the RotWK vertex-building ABI. Verify the native renderer and upload/lock paths before replacing them. Native draw submission and D3D calls remain on the existing render path.

Compatible FX draw batching can also eliminate per-system state and submission work on one core. Preserve transparency order and flush at incompatibilities; do not merge effects merely because their textures match. This and geometry preparation overlap, so their projected gains must not be added independently.

## Movement and other systems

Pathfinding can become another large offload, but its boundary is harder. The existing request queue uses shared budgets and world state. Searches also need isolated frontier and cell scratch. One accepted result may alter inputs for the next request. Snapshot the navigation/occupancy inputs, use private scratch, preserve request order at commit, validate the snapshot version and recompute stale results. Merely running searches concurrently and committing them in order does not reproduce the sequential algorithm.

First reduce repeated repaths, congestion searches and duplicate horde queries without changing decisions. A verified immutable passability/zone snapshot can later support worker searches. These are larger movement targets than another heap instruction tweak.

Culling/visibility and terrain geometry rebuilds are additional candidates if their inputs are frozen and their outputs private. They need stable output order and measured rebuild demand. General object updates, script actions, particle emission/lifetime and complete managers are poor initial worker boundaries because they mix callbacks, allocation, RNG and shared engine state.

## Verification and remaining limits

The final fixture passed against both supplied images on one through four allowed physical cores. It compared 16,136,576 particle-color outputs, including sentinel tails, empty and irregular systems, all optional color/alpha combinations, NaN payloads, signed zero and four SSE rounding modes. It compared 364,032 native bone outputs across x87 single/double/extended precision and four rounding modes, including several root rotations and fractional/wrapped animation frames. Shared clip bytes remain unchanged and read-only. Unmasked FP modes and undersized native batches are rejected by the experimental pool.

The shipping preparation pool is SSE-only. The separate test pool preserves worker x87 environments and joins masked x87 exception flags plus MXCSR flags. These tests compare output bytes, control/tag state and exception bits; they do **not** establish complete x87 condition-code equivalence for a live hook, every animation format, captured/slaved bones or arbitrary engine callbacks. Resolve those contracts before installing native worker hooks. The experiment also omits live object destruction, visibility collection, version checks, commit copies, active D3D submission and GPU contention. Confidence is high in the isolated workload result and moderate in its direction for a runtime port; whole-game gain remains unmeasured.

Identical checked native regions in both images:

| Region | Address / bytes | FNV-1a |
| --- | --- | --- |
| Native raw-animation tree update | `00563310` / 2,850 | `FFFABD60` |
| Animatable tree dispatch including jump table | `005A5050` / 345 | `4B906A19` |
| Scene render boundary | `0047177E` / 733 | `5E1E397E` |
| Root matrix-to-quaternion dependency region | `00B2BD10` / 512 | `304D3B07` |

The preparation tool also checks the complete `.text` identity and its existing particle-hook boundaries. These new reference regions do not authorize installation of any render or animation hook.

Reproduce from an x86 Native Tools prompt, or set `AOTR_VCVARS32` to `vcvars32.bat`:

```powershell
py src\prepare_particle_retail_test.py --retail C:\RotWK\lotrbfme2ep1.exe --offload --output "$env:TEMP\rotwk-offload-exe.bin"
src\build_rotwk_offload_test.bat "$env:TEMP\rotwk-offload-exe.bin" benchmark
```

Repeat with `C:\Users\vvval\Downloads\game (1)\game.dat` and a separate temporary output. Generated native image bytes stay outside the repository. Reference checkouts were read without changes: Open-BFME-1 `d5dc2643a2420b720554c148a4868de4fa02532c`; Open-BFME-2 `2786d73a13b2228753e49b937519925679556abc`. Relevant local source: `animobj.cpp`, `htree.cpp`, `hrawanim.cpp`, `part_buf.cpp` and `pointgr.cpp` under Open-BFME-2's WW3D2 directory.
