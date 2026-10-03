# ROTWK accelerator: late-game performance and cores 2–4

Audited on 2026-10-03. The target is the uncapped, FPS-patched `lotrbfme2ep1.exe`. The supplied `game.dat` is an engine reference. The strongest next candidates are particle geometry preparation, model pose/bounds work, render-list and draw preparation, and audio request indexing. Movement needs its own measurements: faster rendering alone cannot solve expensive pathfinding, formation updates, or target searches.

Follow-up: [implemented capabilities and synthetic measurements](rotwk-synthetic-implementation-2026-10-03.md), including extra-core packing, ID churn and candidates kept disabled after regressions. The capability inventory below describes the pre-implementation audit snapshot.

Aim for 120 FPS by reducing the work that grows with troops and effects, then distributing independent preparation work. The present accelerator does not parallelize general particle simulation or movement. A second rendering thread is already implemented; cores 3 and 4 should first handle model and particle preparation with private outputs. Whole engine update functions are poor initial offload boundaries.

## Evidence and limits

Reviewed the current source, including pre-existing uncommitted loader/direct-EXE changes. Accelerator HEAD is `586fc0e6020eaa334ee41e647b1cacc3221c2d6a`; that revision alone does not describe the working tree. Reference checkouts were read without changes:

| Reference | Revision read | Use |
| --- | --- | --- |
| `../Open-BFME-1/` from the supplied outer workspace | `d5dc2643a2420b720554c148a4868de4fa02532c` | Pathfinding queue, horde update, FX dispatch |
| `../Open-BFME-2/` from the supplied outer workspace | `2786d73a13b2228753e49b937519925679556abc` | Particle buffers, point-group arrays and rendering |

These reconstructions establish useful algorithm and ownership evidence. Their BFME1/BFME2 layouts and addresses are not automatically ROTWK layouts or hook addresses, and reconstruction match claims were not rebuilt here.

| Binary inspected | SHA-256 | `.text` FNV-1a | Preferred base |
| --- | --- | --- | --- |
| `C:\Users\vvval\Downloads\game (1)\game.dat` | `5481de75b63e22483660ea8097478faef594f90b9ade7de893e092256ba68a26` | `4404F3C6` | `00400000` |
| `C:\RotWK\lotrbfme2ep1.exe` | `b1d88fd1456bf9ecaa4bae98e059c1bb6b0de81e882b75ba4e80792277642d8b` | `88C193EE` | `00400000` |

The EXE's FPS patch is accepted as intentional. Both identities already have limited particle/pose capability profiles. The existing reference preparer verifies nine complete routine hashes and eight detour instruction boundaries; the supplied DAT passed those checks again. Short entry-byte comparisons of additional routines are investigation leads, not permission to enable the complete legacy hook set.

**Confirmed** below means observed in source, inspected binary instructions, or tests. **Candidate** means a proposed change requiring ROTWK mapping, profiling and validation. Historical timings in existing comments/documents are identified as such. No live battle, display-FPS capture, GPU capture, replay or multiplayer comparison was run in this audit. No gameplay speedup is claimed.

## What 120 FPS actually requires

120 FPS allows **8.33 ms per rendered frame**. The reported early-game 150–180 FPS corresponds to 6.67–5.56 ms, leaving only **1.67–2.78 ms** for additional battle work before falling below 120 FPS. This supports the view that the engine can render a light scene quickly; it does not establish that a large battle fits the same budget on one core.

| Current heavy frame time, illustrative | FPS | Reduction needed to reach 8.33 ms |
| --- | ---: | ---: |
| 12 ms | 83.3 | 30.6% |
| 20 ms | 50 | 58.3% |
| 30 ms | 33.3 | 72.2% |
| 40 ms | 25 | 79.2% |

These are arithmetic examples, not measurements of this session. A helper running ten times faster only removes most of that helper's share. For example, reducing a 2 ms helper to 0.2 ms saves 1.8 ms; it cannot turn a 30 ms frame into an 8.33 ms frame.

On one physical core, extra CPU worker threads mostly take turns. Single-core improvement must come from fewer scans, fewer copies, cheaper arithmetic, less allocation and fewer draw/state calls. With multiple cores, steady-state frame throughput is approximately limited by the slowest overlapping stage—game logic/preparation, D3D submission, or GPU execution—plus exposed synchronization. Those stages sometimes serialize, so simply taking their maximum is an optimistic model. Measure both total CPU work and the main thread's critical path.

Keep simulation speed, particle lifetimes and animation time independent of uncapped rendering. The legacy six-call logic schedule in this source must be checked against the actual FPS patch before porting its slicer.

## Coverage on this executable

The capability decision is in [initialization](../src/aotr_accel.cpp). A filename does not control hook eligibility; verified code does.

| Feature | Current target coverage | Implication |
| --- | --- | --- |
| Render API worker, staging, resource tracking, retained uploads, fast CRT and preshader accelerator | Portable installation paths, with their own checks | D3D/D3DX execution can overlap game work; CPU geometry generation remains upstream |
| Fused particle RGB/alpha copy/clamp | Enabled when independently checked installation succeeds | Already removes several color-processing passes; still covers full capacity |
| Positive particle-system ID index | Enabled when its grouped lifecycle installation succeeds | Speeds stable-list hits; misses, churn and exceptional layouts retain stock work |
| Per-call HLod matrix reuse | Explicit opt-in, default off | Serial experiment, not a pose worker; prior fixture timings were mixed |
| Full pose workers and bounds warming | Behind legacy `5ED63115` engine gate | Not enabled by the supplied DAT/direct-EXE particle profile |
| Render-list sort/push/erase optimization | Legacy gate | Not enabled on the target profile |
| Audio request index, equivalence/filter memo, picking and engine surface hooks | Legacy gate | Important possible ports, especially long-session audio and repeated queries |
| Logic spreading | Legacy gate | Scheduling optimization, not parallel simulation or a reduction in total update work |
| Per-draw parameter generator | Legacy gate **and default off** | Source documents visible failures in learned recipes; not a ready performance feature |
| Particle simulation / movement workers | No active implementation found | Must design and verify new boundaries |

Do not infer activation from “supported ROTWK” alone. Startup logs should name each installed capability; opt-in prototypes remain experiments.

## Logging fixes made during this audit

The loader already detects direct execution under the selected EXE name and verifies an engine image. The DLL also already falls back from `game.dat` to the main module. The confirmed logging defects were downstream of that detection.

Changes in [aotr_accel.cpp](../src/aotr_accel.cpp), [aotr_rt.inc](../src/aotr_rt.inc) and [aotr_subsys.inc](../src/aotr_subsys.inc):

- Record the actual engine image path and module leaf. Game-range symbols now appear as `lotrbfme2ep1.exe+1A7DF0` when that module is loaded. Legacy DAT processes still report `game.dat`.
- Bound formatted log messages. A long crash-stack record could exceed the old 1,024-byte logging buffer; oversized messages are now truncated into a complete CRLF-terminated record.
- Add reporting-only interval accumulation for portable queued Presents. Previously portable device Presents advanced frame counts without advancing `g_mkPeriod`, producing an unusable zero interval. Swap-chain Presents now use the same portable frame accounting, including functional cache aging. Toggling render mode resets the interval origin. Production retains cache aging and excludes the new reporting clocks.
- Explicitly identify portable phase fields as unmeasured. Queued-Present submission intervals measure the producer's pace while the queued path is active; they are not measured display FPS, and this counter does not cover direct/dirty-region Present paths.
- Restrict the legacy function map and sampler to the verified legacy image. Unsupported targets log the missing mapping instead of attributing samples to old addresses. `AOTR_PROFILE=1` on an unsupported image falls through to normal acceleration instead of selecting legacy engine profiling hooks. Production rejects the dedicated reporting mode.
- Replace the misleading “Everything is installed” claim with capability eligibility wording.

The existing legacy sampler still starts only near 30 ms/frame and samples the 30–250 ms range. It therefore misses 9–25 ms slowdowns that matter for a 120 FPS goal. A subsequent profiler port should use a selectable capture range around the 8.33 ms target, support early-game baseline capture, and identify functions/phases for this executable. This audit gates the unsupported profiler; it does not supply a new ROTWK phase map.

One additional binary finding: the historical, uninstalled “particle” probe at `0044C813` compares a container end/capacity, appends an element, advances the end by 12 bytes, and takes an allocation fallback. That is evidence of a container append, not an established particle-manager update. Its source comment was corrected. Do not offload through that address based on its old label.

## Prioritized single-core work

### 1. Particle geometry: eliminate intermediate passes

**Confirmed reference evidence:** [ParticleBufferClass](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/part_buf.cpp) builds an active-point table, combines color/alpha, supplies arrays to a point group and updates bounds. [PointGroupClass](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/PointGroupClassRender.cpp) compresses position, diffuse, size, orientation and frame into separate arrays when an active-point table exists. Its [array update helper](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/PointGroupClassUpdateArrays00917B10.cpp) then fills locations, UVs and diffuse separately. This establishes multiple passes in the reference implementation; their exact ROTWK callees and live cost remain to be mapped.

**Candidate:** Gather each selected particle directly into its final vertex layout: indexed position, color/alpha, size, orientation and texture frame in one ordered packing pass. Use a shared immutable quad-index pattern where applicable. Reserve reusable output capacity outside the hot loop. This is bulk data processing with fewer temporary arrays and copies, and can benefit one core before adding threads.

Preserve active-point order, triangle/quad choice, camera transform arithmetic, texture-frame mapping, line/trail topology and all consumed capacity tails. The current full-capacity color hook deliberately preserves tails. Replacing `MaxNum` with live count globally is unsafe: line-group code has full-capacity tail-diffuse consumers. Start with a proven point/quad path and keep stock fallback for other modes.

Measure selected particles, configured capacity, packed bytes, temporary-copy bytes and CPU time per geometry stage. High capacity with low live count distinguishes wasted capacity work from actual particle load.

### 2. Particle lookup under constant creation/destruction

**Confirmed:** [The current ID index](../src/aotr_rotwk_particleindex.inc) waits for eight queries without mutation, indexes at least 32 nodes, and stops building above 4,096 nodes or on excessive probe collisions. Insertion invalidates the index; relevant erase/clear/assignment/save-load changes also invalidate it. Misses always take the stock list walk.

**Candidate:** Count index hits, stock fallbacks, rebuilds, failed builds, misses and queries between mutations in a battle. An emission-heavy workload may rarely reach the stable-list fast path even though a synthetic stable lookup is very fast. If confirmed, make insertion/removal maintain the index incrementally, retaining the ordered stock list and weak-handle construction. Preserve first-node behavior for duplicate IDs, observer nulling, ID reassignment/reuse and reentrant loading.

Treat this as a single-core data-structure fix. Multithreading the shared handle list would increase the ownership problem. Negative-result caching is a separate lifecycle problem and needs its own proof.

### 3. Audio backlog: investigate an existing quadratic candidate

**Confirmed source evidence:** [The existing audio index](../src/aotr_audiolimit.inc) targets a request-limit check that walks the request list inside an outer list pass. Comments describe queued looping sounds retried every frame and historical growth from 1% to 7.5% of game-thread time in an older battle capture. Those timings are not measurements of this EXE.

**Candidate:** Map and port this capability independently. Count pending requests, retries, list visits and milliseconds after several minutes of combat. This can explain slowdown that grows with session age and persists after effects leave the camera. Preserve sound limits, positional audibility, retry order and erasure; replacing an exact index with dropping sound requests changes behavior.

This belongs near the top of the investigation even when the symptom is described as “too many FX”: FX can also generate audio requests.

### 4. Models, poses, bounds and repeated pass work

**Confirmed:** [Pose workers](../src/aotr_posewarm2.inc) exist for the legacy build. They qualify owned objects, compute tree/HLod updates, verify outputs, and can warm mesh bounds. They are absent from this target's active capability set. The separate [retail matrix prototype](../src/aotr_rotwk_pose.inc) does not provide this offload and remains default off.

**Candidates:** First measure tree update, HLod child transforms, model/world bounds, skin-palette preparation and mesh-list traversal. Port independently verified pose/bounds preparation. On one core, exact dirty-state/version reuse between shadow/main passes may save duplicate computation; a pose cache must include animation state, blending, pivots, ownership and callback-driven changes. A geometry skinning cache is useful only if profiling proves the same CPU deformation is repeated; do not assume that all skinning happens on the CPU.

Repeated soldier meshes offer a longer-term instancing opportunity. Static attachments or unanimated props are a simpler starting point than animated troops with different bone palettes, damaged states, textures and materials. Shared mesh resources do not imply a shared pose. Cross-object pose reuse needs exact animation/pivot identity and phase; quantizing animation phases changes visuals.

### 5. Draw batching and queue records

**Confirmed:** [The render queue](../src/aotr_rt.inc) batches effect parameter writes, and the [render-list optimization](../src/aotr_rlsort.inc) reduces sort/reference traffic for the legacy build. These do not automatically combine multiple meshes or particle systems into one draw. Retained uploads already remove one large producer copy; native buffer upload remains.

**Candidates, in order:** Port verified render-list sort/push/erase work; avoid redundant binding traversal with a fully specified and invalidated cache; then combine adjacent compatible particle draws into larger vertex/index ranges. Include render pass, effect/technique, texture, sampler, blend/depth state, topology, vertex format, transforms and shader constants in compatibility checks. Engine-level preparation is preferable to blindly merging arbitrary recorded D3D calls.

Keep transparent draw order. Even additive draws should start with order-preserving concatenation and image comparison: depth writes, alpha channels, clipping and floating-point blending can make apparent reorder freedom misleading. Opaque render-list grouping must retain pass semantics. Stop batching at any observable incompatible state or resource dependency.

Microsoft's D3D9 guidance supports larger primitive batches, fewer state changes and proper dynamic-buffer usage. `DISCARD` abandons the whole dynamic buffer, while `NOOVERWRITE` promises not to touch bytes still in use; apply them only with proven resource ranges and lifetimes. [Direct3D 9 performance guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d9/performance-optimizations).

Do not enable the current `AOTR_DRAWGEN=1` as a shortcut. [Its installer](../src/aotr_drawgen.inc) explicitly leaves it off because learned recipes sometimes produced visible geometry failures. The sampled checks do not prove every subsequent draw correct. A replacement needs complete dependency/lifetime keys before deployment.

### 6. Movement, formations and queries

**Confirmed reference evidence:** [The BFME1 pathfinding queue](../../../Open-BFME-1/game/GameEngine/Source/GameLogic/Pathfinder/PathfinderProcessPathfindQueue.cpp) refreshes zones, processes an ordered 512-entry request ring and calls each unit's pathfind routine. It shares a work budget. [The horde update](../../../Open-BFME-1/game/GameEngine/Source/GameLogic/Object/Update/AIUpdate/HordeAIUpdate_update.cpp) checks an existing formation refresh condition and then calls horde and AI updates. These are ownership/order clues, not ROTWK hook ABIs.

**Candidates:** Track requests per update, queue age, cells expanded, blocked/repeated requests, zone refreshes, formation solves, collision candidates and target/filter scans. Optimize search scratch allocation and locality; index repeated exact template/filter answers with complete keys; reuse formation computations only when all relevant inputs are unchanged. The existing formation throttle means a new generic “update formations less often” rule may duplicate stock behavior or alter movement.

Spatial broad-phase indexing can reduce large candidate scans if the engine actually repeats them. Update it at the same logical mutation points and retain exact final collision/target tests. Sharing a horde's coarse route can be useful only after member-specific collision, size, locomotor and goal handling are established. Reusing yesterday's path or lowering AI update frequency changes behavior and is a different feature.

For path caches, key terrain/passability changes, gates/buildings, movement layer, locomotor, unit dimensions, start/goal and any dynamic occupancy inputs the search reads. Keep tie-breaking and search expansion order. A shorter queue achieved by processing fewer requests is not an algorithmic gain.

### 7. Allocation and long-session growth

**Confirmed:** The allocator swap changes named CRT imports. It does not establish coverage of every internal engine pool, STL allocation path or particle allocation. The upload retention pool uses its own bounded storage.

**Candidate:** Measure allocations by owner/size, live system counts, queued audio, cache occupancy, upload pool pressure and committed memory over a long battle, then after units/effects are removed. Distinguish legitimate load from retained objects, backlog and repeated rebuilds. Use bounded reusable arenas for packing/jobs; avoid per-particle/per-unit allocations in new fast paths.

The process remains 32-bit. Replicating large geometry/snapshot buffers for every worker can erase gains through memory pressure. Keep worker storage bounded, reuse it, and preserve stock failure/fallback behavior. Revisit report-only counters/timers in legacy pose/list hooks when porting; several remain outside `PSTAT` in those files.

## What to put on cores 2, 3 and 4

Use physical-core topology and the process's allowed CPUs. Logical CPU numbers are not guaranteed physical core IDs or SMT sibling pairs. The existing render placement hint is compiled only under `AOTR_V4_IDEAL`; the normal production build does not define it. The legacy pose pool uses `logicalProcessorCount / 2 - 2` with at least one worker, which misestimates non-SMT and small systems.

| Available physical cores | Proposed work distribution | Required behavior |
| --- | --- | --- |
| 1 | Main performs engine work and preparation; benchmark direct versus queued rendering | Disable additional compute workers; prioritize less total CPU work |
| 2 | Main: simulation and render preparation. Second: existing D3D/D3DX queue execution | Avoid a separate continuously active compute pool competing for the same cores |
| 3 | Main + dedicated render worker + one shared preparation worker | Worker handles qualifying poses or particle packing; main also takes jobs |
| 4 | Main + dedicated render worker + two shared preparation workers | Balance model/particle jobs by measured cost; reserve D3D order for the render worker |

These are scheduling roles, not mandatory hard affinity. Start with topology-aware soft placement. Compare SMT, allowed affinity and hybrid-core placement on the actual machine. Do not reserve a separate permanent thread for every subsystem.

| Work | Offload suitability | Worker input/output and commit rule |
| --- | --- | --- |
| Existing D3D/D3DX execution | Already implemented | Keep one ordered executor and existing lifetime/device-loss rules |
| Qualified skeleton/HLod transforms and bounds | First model candidate | Frozen animation/transform inputs; exclusively owned tree/subobjects; join before engine consumers |
| Particle billboards, colors, UVs and packing | First particle candidate | Immutable selected-particle/camera inputs; private vertex output; concatenate/submit in stock system order |
| Per-system visual interpolation/bounds | Promising after call-graph proof | Private or exclusively owned arrays; preserve floating-point state, timestamps and bounds reduction order |
| Culling or render-key preparation | Conditional | Immutable bounds/material snapshot; produce visibility/keys; stable serial merge before submission |
| Particle emission, creation/deletion and FX dispatch | Keep serial initially | Shared RNG, system IDs, lists, attachments, audio and callback side effects must be traced first |
| Path search math | Advanced candidate | Immutable world/query snapshot and private search state; original request ordering and serial result commits |
| Unit/horde movement, collision resolution and combat decisions | Keep serial initially | Shared occupancy, state machines, attacks, deletion and deterministic ordering |
| Audio device work | Lower priority | Separate device execution only if profiling warrants it; request-list indexing should come first |

D3D9 does not become efficiently parallel merely by allowing multiple threads to issue device calls. It also has special window/message and reset/presentation threading constraints. Keep CPU preparation workers away from the device and preserve the existing message-aware queue waits. [Microsoft's D3D9 threading rules](https://learn.microsoft.com/en-us/windows/win32/direct3d9/multithreading-issues).

### A concrete first offload boundary

For particle geometry:

1. Main advances systems, performs emission/lifetime decisions, selects active particles and establishes camera/render order.
2. Main publishes bounded jobs whose input storage stays valid until all readers finish. Begin with borrowed immutable arrays during a joined phase; measure snapshot copying before adopting full copies.
3. Workers gather/pack vertices into disjoint output ranges. Give each worker private scratch; reference point-group globals and static compression arrays cannot be used concurrently.
4. Main joins before stock consumers can mutate or release those inputs. It emits the packed ranges in original order through the existing render queue.
5. The render worker performs native uploads and device calls; output storage is retired only after its last queued read.

This gives cores 3–4 arithmetic work without parallel mutation of the manager. A future longer overlap can snapshot a complete render state, but adds copying, lifetime and latency costs. Do not silently show an old particle/model state one frame later to avoid a join.

Use one shared preparation pool with generation-tagged jobs, explicit publication/completion, bounded storage and orderly shutdown. Chunk work by measured size, rather than one interlocked task claim per particle. Wake workers only when expected compute savings exceed dispatch/join costs. Main participates and handles small batches serially. Carry x87/MXCSR controls into jobs; audit sticky flags and callbacks where their effects are observable. Bounds reductions must preserve stock exceptional-value behavior and ordering if bit identity is required.

For parallel path searches, serial commit alone is insufficient: a prior request may change data that the next request reads. Validate snapshots in original order and recompute dependent/stale searches, or prove requests independent. Preserve node-expansion budgets, tie-breaks, RNG and request eligibility. This is a larger project than particle packing or pose preparation.

## Rendering limits that more CPU cores cannot remove

Many overlapping translucent particles may be limited by GPU pixel/blend work. Larger batches reduce submission cost but do not reduce the number of shaded/blended pixels. Compare the same effect-heavy view at different resolutions and with separate diagnostic toggles for shadow/particle rendering; resolution sensitivity is a clue, not proof, because fixed GPU and CPU costs also exist.

Use exact culling and verified bounds first. Distance particle LOD, fewer particles, shorter lifetimes, reduced shadow casters, simplified model LOD and lower-resolution particle passes can improve load but visibly change quality or behavior. Keep them explicit quality options rather than counting them as identical-output accelerator gains. GPU particle simulation or animation instancing requires renderer/shader work and a separate visual/determinism specification; D3D9 offers no drop-in compute offload.

## Measurement and implementation sequence

| Order | Concrete deliverable | Evidence needed to keep it |
| --- | --- | --- |
| 0, completed here | EXE-aware log identity, bounded messages, portable reporting intervals and verified legacy profiler gating | Reporting/production tests and builds pass |
| 1 | Target-specific coarse phase map and selectable capture range | Valid measurements around 8.33 ms; comparable baseline with diagnostics off |
| 2 | Audio index and stable render-list capability ports | Identical results/lifecycle, real late-battle time saved |
| 3 | Particle direct packing and measured ID-churn improvements | Exact selected geometry/lookup results; fewer bytes/passes or walks |
| 4 | Pose/bounds preparation capability, then one/two shared workers | Owned writes, exact outputs, less main-thread critical-path time after joins |
| 5 | Adjacent compatible particle draw batches and dynamic upload ranges | Image/order/resource equivalence; fewer native calls and measured submission savings |
| 6 | Movement/query data-structure improvements | Same commands, paths, combat state and update sequence; reduced search/query work |
| 7 | Snapshot-based parallel path math / broader simulation offload | Dependency validation, deterministic serial commit, multiplayer/replay equivalence |

This order is a working priority, not a substitute for a capture: a pathfinding-dominated battle should move step 6 earlier, while GPU-dominated effects need rendering/quality work.

Use the same FPS-patched EXE, mod, settings, map, replay/save, camera and troop counts. Keep the patch present in every comparison. Capture at least an empty/light scene, dense stationary troops, mass movement through a choke, dense combat FX and a long session followed by removal of units/effects. Include camera-away and audio diagnostic comparisons to expose off-screen work and backlog.

Record median, p95/p99 and maximum frame intervals, percent of frames over 8.33 ms, per-thread CPU work, main-thread waits, render-queue age/occupancy, worker dispatch/join cost, native draw/state/upload counts and relevant system/request counts. Measure simulation ticks per wall second and logic substep spikes separately from rendering. State whether intervals come from RenderViews, queued Present, native Present completion or displayed frames.

Compare stock+FPS-patch, current accelerator, and each candidate independently; then compare the combined build. Warm up proof checks and report startup versus steady-state separately. Run 1/2/3/4 **physical-core** cases with identical allowed CPU sets for comparable A/B runs. A single-core claim needs all game/render workers constrained to that physical core, not just the main thread. Avoid high-rate suspended-thread sampling or per-particle clocks in the final FPS comparison.

## Validation performed

- New [diagnostic identity fixture](../src/diagnostic_identity_test.cpp), reporting and production: actual module identity, EXE/DAT symbol formatting, unsupported-map exit before sampler allocation, long-log truncation followed by a valid record, portable frame accounting, and exclusion of reporting clocks from production.
- Existing render-queue overhead fixtures, both modes: waits, sent-message pumping, scoped calls, ordered execution, ring/sequence wrap and watchdog behavior passed.
- Supplied DAT reference preparation: full code hash, nine complete body hashes and eight detour boundaries passed.
- Existing native ROTWK fixtures, both modes: 480 color cases, 600 HLod cases, lookup/observer/lifetime cases, foreign-thread fallback and six partial-install rollback cases passed. These fixtures run private child mappings, not a battle.
- Full x86 production and reporting DLL builds passed with freshly compiled rpmalloc. Game executables were not modified or launched for this audit; distribution binaries were not replaced.

Reproduce the source checks from the inner repository root:

```powershell
$env:AOTR_VCVARS32 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
& .\src\build_diagnostic_identity_test.bat
& .\src\build_render_queue_overhead_test.bat
$reference = Join-Path $env:TEMP 'rotwk-audit-fx-reference.bin'
py src\prepare_particle_retail_test.py --retail 'C:\Users\vvval\Downloads\game (1)\game.dat' --output $reference
& .\src\build_rotwk_fx_test.bat $reference
& .\src\build_new.bat
& .\src\build_prod.bat
```

Check each command's exit status before continuing. The generated reference contains game sections and belongs in Temp; do not distribute it. These checks establish the logging change and existing helper behavior. Sustained 120 FPS, safe new offloads and a single-core late-battle target remain unproven until the controlled gameplay measurements are available.
