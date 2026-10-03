# FX and particle audit

Audited on 2026-10-03. The reported slowdown seems to track the number of FX rather than persistent auras. The strongest source-backed candidates are particle update/geometry work, repeated system-ID searches, full-capacity color processing, upload copies, and the cost of drawing many overlapping effects. Which dominates in the affected game/mod still needs a battle capture.

The accelerator currently moves rendering API work to a worker and reduces some shader parameter/state work. It does **not** currently offload particle simulation. Adding particle-specific measurements and optimizing verified CPU paths would extend its coverage substantially. Reducing particle counts or changing visual quality is a separate option, with visible consequences.

## Scope and confidence

Read the accelerator at `f92495f37e6a9c6d68471236697feb8fd6583a15`, Open-BFME-1 at `6e142e55963c440af5d437f0895209960ca8bd57`, and Open-BFME-2 at `d8bb897b3a63cbb3cc7257ce3640e708c4750793`. Used the separate Open-BFME-1 checkout, rather than Open-BFME-2's modified reference submodule. Neither reconstruction workspace was changed.

Checked selected BFME2 `matched` ledger rows and inspected function-entry instructions in the local vanilla 1.06 `game.dat`. This confirms useful binary locations for further investigation; it does not validate a new detour or prove that every game FX uses those functions. Existing reconstruction match claims were not recompiled. No live game, replay, GPU capture, or FPS comparison was run. Impact below means plausible workload benefit, not measured gains.

The audit follows three distinct layers:

| Layer | Work | Accelerator coverage today |
| --- | --- | --- |
| Game FX dispatch | FXList aliases, shroud checks, nuggets, bone effects, spawning secondary effects | No dedicated replacement found for these dispatch paths |
| Particle engine and WW3D | Emission, lifetime, position/module updates, bounds, color/size/rotation, geometry | No active particle simulation offload; general buffer/API optimizations help the upload/submission end |
| D3DX effects and device | Shader parameters, preshaders, render state, resource uploads, draws | Render queue, effect parameter batching/deduplication, buffer staging, and other existing shortcuts |

An `ID3DXEffect` parameter batch is not a batch of game FX systems. WW3D has point/line rendering and shader-state preparation of its own. Measure the paths actually used by each effect before assigning all particle cost to D3DX.

## What the code establishes

### 1. The particle offload mentioned in the accelerator is unfinished

[The particle section](../src/aotr_accel.cpp#L413) defines a signature, original-function pointer, timing wrapper, and counters. No installation of that wrapper or assignment of its original pointer was found. Its comment proposes a later split across workers; that split is absent. [Initialization](../src/aotr_accel.cpp#L2714) also describes the old particle measurement hooks as disabled.

The signature's comment names a RotWK/AotR address. It is not evidence that the body is a complete particle-manager update, and it supplies no BFME2-compatible ABI. Recover the caller/body and verify each supported binary before using it.

There are already general stage, sampling, and render-queue diagnostics, including particle labels. They are not a complete per-system profiler. The function table and [particle-manager sampling focus](../src/aotr_accel.cpp#L2391) use fixed addresses; BFME2 diagnostics need their own verified mappings. The earlier audit also records production counter/report limitations.

**Potential impact:** High value for choosing the right change; no direct FPS gain from instrumentation itself. Avoid timing every particle with QPC or suspending the game thread at high frequency during performance comparisons.

### 2. System-ID lookup scales with the number of systems

BFME2's [findParticleSystemByID](../../../Open-BFME-2/Code/GameEngine/Source/Common/ParticleSystemManagerFindByID.cpp) walks the manager's list and compares each system's ID. BFME1 has [the same general search](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/ParticleSystemManager_findByID.cpp), with a different layout. Many repeated lookups over many systems can compound into a significant CPU cost.

Add a manager-owned ID index only after tracing insertion, removal, deferred destruction, reset/load, and ID reuse. Return the same owning handle as the stock path. Preserve invalid-ID and miss behavior; an index of raw pointers without lifecycle coverage could return a destroyed system. Keep the stock list for traversal and ordering. Count calls, list nodes visited, misses, and active systems first.

This is independent of PR 3's D3D resource-tracking index. It needs its own change and validation. BFME2's [particle-template lookup](../../../Open-BFME-2/Code/GameEngine/Source/GameClient/System/ParticleSystemManagerFindTemplate.cpp) already uses a bucket table; replacing that with another hash table is not an established improvement.

**Potential impact:** Medium to high when system-ID lookup is frequent and many systems coexist; little benefit if lookups are rare. Reduces lookup work without reducing effect counts.

### 3. Some rendering work follows capacity rather than live count

In BFME2's [part_buf.cpp](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/part_buf.cpp), `Combine_Color_And_Alpha` copies and clamps **MaxNum** entries when color or alpha arrays exist. `Render_Particles` calls it before passing the active point table to the point group. A large configured capacity with few living or selected particles can therefore still incur a large color pass.

Candidate: update only entries downstream rendering actually reads, or fuse color/alpha conversion with vertex packing. Before changing it, establish every consumer of `Diffuse`, including line-group rendering and APT handling. Preserve the circular-buffer indices, newly emitted entries, clamp behavior, defaults, and LOD selection. A packed output array cannot simply replace an indexed source array.

**Potential impact:** Potentially high for sparse, oversized particle buffers with animated color/alpha; smaller for nearly full buffers. This is a more concrete target than a general request to multithread all particles.

### 4. Particle updates and geometry contain several count-dependent passes

The same BFME2 source contains living-particle position/velocity work, visual keyframe interpolation, active-point selection, bounds scans, and color processing. More particles can increase several CPU passes before a draw reaches the API queue.

Some shortcuts already exist: kinematic updates return when elapsed time is zero, bounds use a dirty flag, constant visual state bypasses interpolation, and complete LOD decimation skips visual updates. Preserve these and measure where time remains. Do not add another cache solely because bounds or updates are called from multiple paths.

Candidates are tighter geometry packing, specialized constant-property paths where not already covered, and SIMD in verified arithmetic loops. Compare identical inputs and float results, not just screenshots. Changing x87/SSE precision, expression grouping, conversion rounding, or keyframe selection can change particle motion and appearance. Visual keyframe cursors advance through particles in age order across the ring; arbitrary parallel chunks need correctly initialized independent cursors.

BFME1's [point-group renderer](../../../Open-BFME-1/game/Libraries/Source/WWVegas/WW3D2/PointGroupClassRender.cpp), [UV fill](../../../Open-BFME-1/game/Libraries/Source/WWVegas/WW3D2/PointGroupClassUVFill.cpp), [diffuse fill](../../../Open-BFME-1/game/Libraries/Source/WWVegas/WW3D2/PointGroupClassDiffuseFill.cpp), and [submission](../../../Open-BFME-1/game/Libraries/Source/WWVegas/WW3D2/PointGroupClassSubmit.cpp) provide useful algorithm evidence. Its scratch arrays are shared. Open-BFME-2's [similarly named Render file](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/PointGroupClassRender.cpp) explicitly omits the differing BFME2 Render definition and carries an APT helper. This is a recovery gap, not a ready-made BFME2 renderer replacement.

**Potential impact:** Medium to high if CPU update or vertex generation dominates. D3D call offload alone does not remove these passes. Start with a verified single-thread optimization; introduce workers only for isolated arithmetic over stable snapshots and private output buffers.

### 5. Many FX events can produce considerably more systems and particles

BFME2's [bone FX nugget](../../../Open-BFME-2/Code/GameEngine/Source/GameClient/FXListAtBonePosFXNugget_doFxAtBones.cpp) gathers up to 40 bone positions and invokes an FXList for each returned bone. That is an upper bound in this helper, not a claim that every effect uses 40 bones. Its [FXList dispatch](../../../Open-BFME-2/Code/GameEngine/Source/GameClient/FXListDoFXPos.cpp) resolves aliases, applies shroud gates, tests nuggets, and stops when a nugget consumes the event.

BFME1's [emission](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/ParticleSystem_emit.cpp) can create an attached system per accepted emitted particle and recursively emit through a slave system. Its [terrain-collision module](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/FXParticleSystem/ParticleTerrainCollisionModuleUpdate.cpp) can trigger another FXList. These establish amplification mechanisms; trace the equivalent BFME2/RotWK call paths before assigning their cost to a specific mod.

Measure parent/child system counts and spawn rates by template and FXList. Cache immutable alias resolution or bone-name lookup only with template/model generation invalidation. A bone's current world transform changes with animation; a name cache cannot replace that transform. Do not deduplicate FX events or skip nugget tests: sound, secondary effects, shroud rules, RNG, and consumption order can matter.

**Potential impact:** High diagnostic value for identifying a few templates that generate most work. Exact lookup improvements can help; reducing emission or attached effects changes presentation.

### 6. Buffer staging removes waits but retains copies and memory costs

[rtBufLock/rtBufUnlock](../src/aotr_rt.inc#L1869) stage compatible VB/IB writes. Unlock copies staged bytes into a queue payload; the worker later [copies them into the driver mapping](../src/aotr_rt.inc#L491). Plain-lock mirrors can add copies in both directions and a synchronous whole-buffer read on first adoption. DISCARD invalidates a mirror, so mixed lock patterns may cause repeated adoption.

The staging arena is 64 MiB; individual staged writes are limited to one eighth of the 32 MiB queue buffer. Plain buffer mirrors have a separate **320 MiB ceiling**, not a claim that this much is always allocated. These paths also serve UI and terrain; global lock counts alone do not identify particle cost.

Add uploaded bytes, copy time, lock flags, mirror adoption/drop, and fallback reasons grouped by verified particle callsite/resource. If copying matters, let queued upload records retain stable staging blocks until worker consumption, removing the staging-to-payload copy. Current arena reuse on unlock makes simply queuing its pointer unsafe. Preserve lock ranges, DISCARD/NOOVERWRITE semantics, resource references, queue order, reset/toggle handling, failure fallback, and bounded memory use.

**Potential impact:** Medium to high for upload-heavy CPU frames; low when geometry generation or GPU fill dominates. Fix the general queue-size failure handling from the [main audit](accelerator-audit-2026-10-03.md) before making upload records larger or changing ownership.

### 7. Submission and overlapping transparent pixels need separate measurements

Many small systems can mean many state changes and draws; large overlapping smoke/fire/explosion sprites can also process substantial screen area. The source identifies rendering paths, but this audit does not establish a GPU bottleneck in the user's battle.

Compare the same effect-heavy scene at different resolutions and camera distances, keeping gameplay and settings constant. A resolution-sensitive slowdown suggests a pixel-work component; it does not prove particles are the only source. Add GPU timing without immediate blocking query reads, and compare CPU preparation, worker submission, GPU duration, and presentation wait separately.

Adjacent compatible draws may be mergeable after validating transforms, material state, topology, indices, and exact order. Sorting transparent draws by texture to reduce state changes can change blending. Instancing, a GPU simulation rewrite, or reduced-resolution particle rendering would need new renderer/shader work; they are not small extensions of the current queue.

**Potential impact:** High if tiny-draw overhead dominates. CPU optimizations have limited effect when GPU pixel work dominates. Reduced-resolution rendering or count/size changes can help that case but alter quality.

### 8. First-use resource creation can produce a different kind of hitch

BFME2's [particle texture loader](../../../Open-BFME-2/Code/GameEngineDevice/Source/W3DDevice/GameClient/BFME2LoadParticleTexture.cpp) first checks an existing prototype and creates one on a miss. This is not evidence of loading a texture from disk for every particle. Hit-path options are also updated, so an extra name-only cache must not bypass observable behavior.

The accelerator's [D3DX creation/loading scopes](../src/aotr_rt.inc#L1943) drain the queue and serialize device access. Their timing includes the drain, so it cannot all be charged to decoding or shader compilation. Compare the first cast with repeat casts and record creation misses plus drain time. Selective prewarming at an appropriate load phase is worth considering only if first-use hitches are observed and actual resource creation is traced. Respect ownership and the 32-bit memory budget.

**Potential impact:** Medium to high for confirmed first-use hitches; little effect on sustained slowdowns from many already-running effects.

## Correctness boundaries

- **Hidden is not dead.** BFME1's [ParticleSystem::update](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/ParticleSystemUpdate.cpp) suppresses emission under several hidden/shrouded conditions but still updates existing particles afterward. Skipping the system would change aging, destruction, attachments, and its appearance when visible again. Verify the target build's equivalent behavior before culling work.
- **Modules are not independent arithmetic by default.** BFME2's [wind update](../../../Open-BFME-2/Code/GameEngine/Source/GameClient/System/FXParticleSystem/Rva003A5572Update.cpp) changes module state and uses shared client random functions. Terrain queries, FX spawning, list mutation, and shared renderer scratch arrays also constrain parallelism. Moving the whole loop to workers can change order and race engine state.
- **Existing limits have semantics.** BFME1's [particle creation](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/ParticleSystem_createParticle_Bfme.cpp) checks budgets/LOD, and [oldest-particle removal](../../../Open-BFME-1/game/GameEngine/Source/GameClient/System/ParticleSystemManagerRemoveOldestParticles.cpp) walks priority buckets. Preserve priority and eviction policy rather than inventing a blanket discard rule.
- **Names can mislead.** BFME2's [FXParticleSystemLineDraw](../../../Open-BFME-2/Code/GameEngine/Source/GameClient/FXParticleSystemLineDraw.cpp) draws emission-volume debug endpoints, not a recovered production lightning/streak renderer. Default-name constructors are startup work, not established frame hotspots. Do not hook either based only on its filename.
- **Shader caching needs the earlier correctness work.** More effects may increase D3DX work, but extending preshader cache reuse before covering all dependencies and invalidations can render stale values. See the main audit's cache finding.

## Proposed work and validation

| Order | Deliverable | Expected impact and condition |
| --- | --- | --- |
| 1 | Verified per-build particle/FX counters and sampled timers; record active systems, live particles, capacities, spawn/destruction, ID search visits, geometry time, upload bytes, draw counts, and CPU/GPU frame time | Identifies whether cost follows systems, particles, capacity, draws, or pixels; required to rank the remaining changes |
| 2 | Active-entry or fused color/alpha processing in the verified WW3D path | Strongest concrete rendering opportunity when capacity greatly exceeds occupancy; preserve every consumer's index contract |
| 3 | Particle-system ID index after lifecycle recovery | Removes list-scan scaling if profiling confirms repeated searches; separate from PR 3 |
| 4 | Specialized/SIMD geometry and update kernels; then selective worker experiments | Helps CPU particle bottlenecks; limited to verified ABI, arithmetic, RNG, ordering, and scratch ownership |
| 5 | Upload ownership/copy reduction and adjacent draw merging | Helps bytes/submission-heavy workloads; requires queue and resource lifetime validation |
| 6 | Prewarm proven first-use resources | Targets spikes rather than sustained high FX counts |
| Optional | Explicit visual-quality budget: template emission, lifetimes, attached effects, caps/LOD, particle screen coverage | Potentially large benefit, but changes effects; keep separately selectable and validate visibility/priority rules |

Use a fixed replay or reproducible battle and camera route with few effects, many simultaneous short FX, and a high live-particle count. Include a variant with many systems but fewer particles per system, and one with fewer systems but more particles, to separate the two costs. Repeat first-use and warm runs. Record frame-time median and p95/p99, not only average FPS, plus game version/mod, executable hash, accelerator settings, renderer/backend, resolution, and hardware. Compare a stock baseline, current accelerator, and one proposed change at a time. Use the same settings and check profiler overhead.

Correctness cases should include burst emission, ring wrap/full buffers, expiration, hidden/shrouded transitions, attached/slave systems, animated bones, collision-triggered FX, line/line-group modes, LOD changes, pause/resume, large elapsed time, system-ID reuse, save/load/reset, and resource release with queued work. Verify particle state against stock for arithmetic changes and capture images/draw order for rendering changes. If affected state can reach gameplay, include replay and multiplayer determinism checks.

The [existing render harness](../src/rt_harness.cpp#L453) exercises a small particle-style dynamic-buffer draw, not engine simulation or realistic FX storms. Extend it for many buffers/locks, queue wrap, lifetime/reset, and upload-order correctness; use the live engine scenarios for emission and module behavior. Keep counters aggregated and report outside hot loops.

## BFME2 locations for follow-up

These are selected existing `matched` rows from `Open-BFME-2/reverse/functions.csv`, not newly validated hook capabilities. All values below are **RVAs**; preferred virtual address is `0x400000 + RVA`. Do not reuse them for RotWK or BFME1.

| Function | RVA | Ledger size |
| --- | --- | --- |
| ParticleBufferClass::Combine_Color_And_Alpha | `0x001A8630` | 357 bytes |
| ParticleBufferClass::Update_Visual_Particle_State | `0x001AA680` | 3254 bytes |
| ParticleBufferClass::Update_Non_New_Particles | `0x001AB670` | 3923 bytes |
| ParticleBufferClass::Update_Bounding_Box | `0x001AECA0` | 614 bytes |
| ParticleBufferClass::Render_Particles | `0x001AEF10` | 326 bytes |
| ParticleSystemManager::findParticleSystemByID | `0x001F5B0A` | 111 bytes |
| ParticleSystemManager::findTemplate | `0x001F90DA` | 43 bytes |
| FXListAtBonePosFXNugget::doFxAtBones | `0x001E2DD6` | 283 bytes |

Before implementation, confirm complete function boundaries, calling convention, layouts, callers, supported-build fingerprints, and safe patch installation. Use clean implementations of the established algorithms; do not copy reconstruction source bodies into the accelerator without resolving their licensing.
