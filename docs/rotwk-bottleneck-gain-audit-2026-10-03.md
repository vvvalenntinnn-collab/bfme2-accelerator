# RotWK bottlenecks and anticipated gains

Audited on 2026-10-03 against accelerator `c8045b56402f170fd637559bf8e67e38e54468f5`, including the current EXE-aware logging and guarded retail capabilities. This report covers serialized simulation, pathfinding, object/drawable updates, dynamic buffers and animation, particles, renderer/terrain work, and scripting.

The strongest remaining candidates are exact pathfinding/query improvements, order-preserving particle batching, and model preparation that avoids duplicate work. Script-name copying is a smaller but unusually concrete lead. There is also an executable-name mismatch in DXVK's BFME2/RotWK profile worth testing if DXVK is used.

I would budget for **10–25% less CPU critical-path work beyond the current accelerator** if several of these candidates account for substantial time in a dense battle. That is an engineering planning range, with low confidence until costs are measured. In an entirely CPU-bound case it corresponds to about **11–33% higher FPS**. A battle dominated by untouched AI, GPU overdraw or a synchronization barrier could improve little. Gains already recorded in the [synthetic implementation report](rotwk-synthetic-implementation-2026-10-03.md) are excluded from this forecast.

## What the evidence establishes

Confidence that a mechanism exists is different from confidence that it dominates this game. Neither the reference architecture nor a development roadmap measures the supplied RotWK build's late-battle frame time.

| Area | Confidence in the mechanism | Confidence in late-battle dominance | Current accelerator coverage |
| --- | --- | --- | --- |
| Main-thread / serialized simulation | Very high | High as an aggregate CPU constraint; its largest component is unmeasured | D3D API worker; retail coarse timers. Legacy logic spreading and pose workers are gated out of these retail targets |
| Pathfinding / horde congestion | Very high | High for mass movement through obstacles; lower for stationary FX | Verified queue timer; no retail path-search replacement |
| Object / drawable updates | High | High candidate as troop count grows; exclusive cost unmeasured | No general retail update-loop replacement; several narrower rendering/query optimizations |
| Dynamic/write-only buffers and animation rendering | Very high for buffer behavior; high for model preparation as a candidate | Driver-, model- and workload-dependent | Staged VB/IB writes, plain-lock mirrors and retained upload storage; retail model matrix prototype remains off |
| Particles / army-wide FX | High, including native synthetic measurements | High candidate for the reported FX symptom; CPU versus GPU unresolved | Fused colors, positive ID index, experimental large-pack workers; gather prototype off |
| Renderer CPU submission / terrain | High for submission; medium-high for terrain work | Medium-high; scene, camera and shadow settings matter | Render worker, state/effect shortcuts, list sorting and draw-opportunity counters; no general terrain replacement or native draw merging |
| Script lookup / updates | High for the upstream copying issue; medium for the equivalent RotWK code | Medium, especially script-heavy maps; unknown on ordinary skirmish maps | No dedicated retail script-lookup hook or exclusive script timer |

Open-BFME's roadmap includes multicore work, but this is a planned capability rather than proof of a specific bottleneck or expected scaling. [Open-BFME-1 roadmap](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/README.md).

Local references were read without modifications: Open-BFME-1 `d5dc2643a2420b720554c148a4868de4fa02532c` and Open-BFME-2 `2786d73a13b2228753e49b937519925679556abc`. Some files mix recovered BFME bodies with Zero Hour donor implementations marked `present-unmatched`. Those portions establish investigation leads, not RotWK behavior or hook compatibility. Match-ledger claims were inspected selectively and were not rebuilt for this documentation audit.

## 1. Serialized simulation and update scheduling

BFME's recovered phase-based logic body runs scripts, commands, object passes, due update modules and subsystem work in an ordered sequence. The scheduler already records wake frames, keeps sleeping modules separate, and handles modules that change their scheduling while updating. This undermines a generic proposal to add sleeping or skip all unchanged objects: the engine already skips some work, and apparently idle modules can still have scheduled effects. [Recovered logic update](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameLogic/System/GameLogic.cpp), [wake scheduler](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameLogic/System/GameLogicAwakenUpdate.cpp).

The accelerator's [logic spreader](../src/aotr_logicspread.inc) retains a verified older build's update sequence while changing where rendering falls. Its historical timings do not apply to the supplied retail EXE. Initialization gates that spreader and the older pose workers behind `g_engineHooks`; the inspected retail targets receive independent capabilities instead. Spreading work can improve uneven frames without reducing total work. Moving the existing entire logic function to another core would still leave one serialized chain and add communication.

The useful single-core targets are redundant exact queries, allocation/reference traffic, and repeated traversal inside due updates. Preserve callback order, wakes, destruction, RNG calls and simulation time. Any object batching must retain the engine's observable order and its handling of callbacks that mutate lists.

Measure logic calls per simulation tick, tick duration, the distribution of work across update phases, and module counts by class. Rendering at 120 FPS does not imply 120 simulation ticks per second. Keep the user's FPS patch throughout comparisons; do not infer a fixed render-to-logic ratio from the older build's comments.

**Anticipated gain:** no separate percentage for “serialized simulation.” It contains pathfinding, scripts and object work discussed below. Counting it again would inflate the forecast. Exact reductions in those components help one core; workers help only eligible independent portions.

## 2. Pathfinding, movement and congested hordes

BFME2's map-design guidance explicitly connects narrow gaps, awkward passages and stray impassable cells with pathfinding slowdown. It supports congestion as a serious test case, but provides no frame-time share or optimization multiplier. [BFME2 Map Design & Beautification Tips, pages 1 and 3](https://www.the3rdage.net/files/6700/BFME2_Map_Tips.pdf).

The BFME1 reference refreshes zones and drains an ordered 512-entry request ring under a work budget. Movement-state code also contains blocked-frame and repath handling. These establish why repeated requests and changing occupancy can grow expensive. [Path queue](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameLogic/Pathfinder/PathfinderProcessPathfindQueue.cpp), [movement-state update](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameLogic/AI/AIInternalMoveToStateUpdate00172E70.cpp).

**New RotWK binary observation:** `006F2364` contains two request-ring lanes at owner offsets `+1C9E8` and `+1C1E0`. Both use `006EC0D1`, whose head wraps after entry 511. The first lane calls AI vslot `+234`; the second calls `+230`. Both resolve an object ID through `00449681`. The routine loads its budget from a global owner's `+11E8` field, gives the earlier lane a half-budget threshold, and tests shared work accounting before continuing the later lane. It also has an early-game budget amplification branch. These are static observations; the semantic names of the two callbacks and the runtime configured budget were not independently established. BFME1's hardcoded budget and single-ring layout must not be transplanted into this target.

Prioritize reusable private search scratch, better locality in the same open/closed-set algorithm, and elimination of repeated exact passability/filter computations. Preserve search tie-breaking, node expansion counts, accounting and result order. An optimized search that uses fewer counted expansions can change how many requests complete in the same update, even when its individual paths match.

Path reuse needs complete keys: terrain/zone and occupancy versions, movement layer, dimensions, locomotor, start/goal, and relevant dynamic obstacles. Sharing a horde leader's route cannot replace member-specific collision and goal handling without evidence. Lowering request frequency, moving work to a later tick, relaxing collisions or changing a map can improve speed, but also changes behavior.

**Anticipated gain:** 15–35% less time in a proven hot pathfinding/query component is a reasonable development target for exact single-core improvements. Confidence is low to medium; there is no native search benchmark yet. At a 35% critical-path share this saves 5.25–12.25% of frame time, approximately 5.5–14.0% more FPS in a CPU-bound case. Independent search workers could offer more, but need isolated scratch and a validated snapshot. Results must be checked and committed in original request order; stale searches must be recomputed.

The queue timer alone misses movement-state, collision, formation and targeting work outside the queue. Add per-lane request counts, oldest request age, expansion counts, zone-refresh cost and blocked/repeated requests, alongside those separate movement costs. Faster frames with growing path backlog would be a failed optimization.

## 3. Per-object and per-drawable overhead

The recovered client update walks drawables, computes visibility-related state and calls `updateDrawable`. The latter performs more than geometry submission: tint/envelope handling, object state, module callbacks and cross-object melee-related work are present. Off-camera rendering culling therefore does not justify skipping the whole drawable update. [Client update](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameClient/ClientUpdate004329D0.cpp), [drawable update](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngine/Source/GameClient/DrawableUpdateDrawable.cpp).

Promising reductions are exact dirty/version reuse of visual transforms and bounds, stable module/filter answers, and reusable temporary storage. Many horde members make a small repeated overhead material. However, stock object-ID lookup is already keyed in the path-queue reference, and BFME2's script unit lookup first tries a stored object ID. A second generic object hash table is not an established improvement. [BFME2 unit lookup](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/GameEngine/Source/GameLogic/ScriptEngine/ScriptEngineGetUnitNamed.cpp).

**Anticipated gain:** 5–20% reduction in exclusive object/drawable overhead after excluding pathfinding, scripts, particles and numeric model work. At a 15% frame share, that is 0.75–3% less frame time, or about 0.8–3.1% more FPS. Confidence is low until counters identify which callbacks and queries repeat. A large payoff requires numerous redundant calls; virtual dispatch alone is not a sufficient diagnosis.

Use equal troop counts with stationary, moving and attacking cases. Separate drawable update, due logic modules, targeting and collision costs, and count calls per object rather than estimating cost from the number of units.

## 4. Dynamic/write-only buffers and animation rendering

### A profile can disappear when the engine becomes an EXE

DXVK's audited configuration enables `d3d9.cachedWriteOnlyBuffers` for BFME2 and RotWK installation paths ending in `game.dat`. The inspected `C:\RotWK\lotrbfme2ep1.exe` path does not match that expression. This is a concrete configuration mismatch if that process uses DXVK with no explicit override. [DXVK configuration at `d30be2b`, BFME profile](https://github.com/doitsujin/dxvk/blob/d30be2baea02a67b9502bffda0ec8b0d915bb3d2/src/util/config/config.cpp#L961).

The option defaults to false and influences host-cached buffer memory selection. It is not an animation cache or a general instruction to add `DYNAMIC` to every buffer. [DXVK option default](https://github.com/doitsujin/dxvk/blob/d30be2baea02a67b9502bffda0ec8b0d915bb3d2/src/d3d9/d3d9_options.cpp), [buffer memory selection](https://github.com/doitsujin/dxvk/blob/d30be2baea02a67b9502bffda0ec8b0d915bb3d2/src/d3d9/d3d9_common_buffer.cpp#L127).

No `d3d9.dll`, `dxvk.conf` or `lotrbfme2ep1_d3d9.log` was found directly in `C:\RotWK` during this audit. That does not establish which DLL a live process loads. If DXVK is in use, explicitly test `d3d9.cachedWriteOnlyBuffers = True` in its configuration and verify the effective option in the DXVK log. Compare DXVK and native D3D9 independently, with accelerator on/off. No configuration or DLL was changed here.

### Most of the obvious upload fix already exists

BFME2's recovered dynamic VB/IB code already creates write-only/dynamic storage and uses `DISCARD` at the start of a range and `NOOVERWRITE` for append ranges. Blanket flag changes would duplicate existing behavior or violate lifetime rules. [Dynamic VB implementation](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/Libraries/Source/WWVegas/WW3D2/bfmedynamicvertexbuffer.cpp), [dynamic IB lock](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/Libraries/Source/WWVegas/WW3D2/DynamicIBAccessWriteLock.cpp).

The accelerator's [buffer hooks](../src/aotr_rt.inc) already stage supported writes, retain a CPU mirror for eligible plain locks, and queue native uploads. [Retained upload blocks](../src/aotr_uploadlease.inc) remove one large producer copy. Remaining copies, native driver locks, unsupported fallbacks, mirror adoption and resource dependencies can still serialize work. Measure bytes, lock flags, fallback reason and queue-drain time by caller. `DISCARD` invalidates the whole buffer; `NOOVERWRITE` requires ranges that the GPU is no longer reading. [Microsoft's D3D9 buffer guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d9/performance-optimizations#using-dynamic-vertex-and-index-buffers).

**Anticipated gain:** 10–40% less exclusive upload/lock/copy cost in a case where avoidable copying or stalls are demonstrated. At a 15% frame share, this is 1.5–6% less frame time, or about 1.5–6.4% more FPS. A pathological driver-memory case may improve more; a correctly configured, already staged path may gain nothing. Confidence is medium in the configuration lead and low in the gain magnitude.

### Animation and skin preparation need a separate measurement

The WW3D renderer reference contains deformation followed by vertex packing into a dynamic buffer, using shared temporary vertex/normal arrays. The skin-render body is marked `present-unmatched`, so it is evidence for a candidate pipeline, not proof that all RotWK models use this CPU path. The recovered mesh flush does establish separate rigid/skinned categories and delayed material passes. [Renderer reference](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/Libraries/Source/WWVegas/WW3D2/dx8renderer.cpp), [recovered flush](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/Libraries/Source/WWVegas/WW3D2/DX8MeshRendererClass_Flush.cpp).

Investigate tree update, animation blending, bone/palette creation, CPU deformation, vertex packing and bounds separately. Reuse an exact pose between main/shadow passes only when all consumed state is unchanged. Shared mesh resources do not imply shared animation phase or bone transforms. Frozen numeric inputs and private scratch make isolated preparation a possible worker job; arbitrary engine callbacks do not.

**Anticipated gain:** 15–35% less exclusive model preparation through proven reuse or tighter arithmetic/packing. At a 20% frame share, this saves 3–7% frame time, or about 3.1–7.5% more FPS. Confidence is low. The existing native HLod matrix prototype measured a regression with different bones, so it remains off; its hoped-for gains must not enter this forecast. Eligible bulk numeric work might improve 1.5–2.5x on main plus preparation workers, after a different boundary is verified.

## 5. Particles, FX dispatch and army-wide effects

There are at least four costs: dispatch/spawn and system lifetime; simulation and visual updates; geometry preparation; and draw submission/GPU blending. The guarded manager timer is not a measurement of every one of these costs. A dense effect can also create an audio backlog; the current audio index is already part of the baseline.

The [particle-buffer reference](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/Libraries/Source/WWVegas/WW3D2/part_buf.cpp) establishes active-index selection and multiple passes, including full-capacity color work. Current fused colors preserve capacity tails and already measured roughly 3.4x with valid inputs. Large joined packing measured 1.77x/2.63x versus serial SIMD with three/four physical cores, respectively. These gains are already available in the default/experimental paths, not new forecast gains. Small systems remain serial, and serial gathering regressed. [Existing results](rotwk-synthetic-implementation-2026-10-03.md).

**New external precedent:** GeneralsGameCode PR #3155 reports particle-rendering improvements, with a 15–30% headline and 15–20% average in its description plus an additional visibility improvement. This is a different SAGE game and test scene; it is not a measured RotWK FPS increase. [Upstream particle batching PR](https://github.com/TheSuperHackers/GeneralsGameCode/pull/3155).

The actual implementation carries compatible points across system boundaries and flushes on texture, shader or billboard changes. It retains draw order and shared point-array capacity. Its description's texture wording does not establish a new render-to-texture atlas: the code retains a texture reference and renders through the point group. [Merged particle renderer at `de20ae0`](https://github.com/TheSuperHackers/GeneralsGameCode/blob/de20ae0cbd29209b273873368836b631789e1193/Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DParticleSys.cpp).

RotWK needs its own compatibility key, including texture frame/UVs, coordinate transforms, shader/material, blend/depth state, point/line mode and pass. Preserve transparent order and stop at incompatible systems. Gather compatible small systems into one private preparation/output batch instead of waking workers for each effect. Do not skip simulation merely because rendering is off-screen. Lifecycle, bounds and future emission can still matter.

**Anticipated gain:** 20–40% less remaining CPU particle preparation/submission cost where many small compatible systems dominate. At a 25% critical-path share, this saves 5–10% frame time, or about 5.3–11.1% more FPS. Confidence is medium in batching as a target, low in this RotWK range. GPU-bound overdraw will not disappear when the same blended pixels are drawn in fewer calls. Selected-particle packing that avoids intermediate arrays remains a separate candidate requiring a benchmark; the current gather prototype is not evidence of a win.

## 6. Renderer CPU submission and terrain

The render worker moves API work but does not eliminate it. WW3D already groups rigid/skinned categories and material tasks. The accelerator already makes list sorting much cheaper; the previous 27x synthetic sort result cannot be multiplied into a new renderer forecast. [Current retail work hooks](../src/aotr_rotwk_work.inc).

Remaining candidates are fewer redundant state/resource traversals, larger compatible geometry ranges and fewer unnecessary synchronization points. The reporting draw-span observer counts a narrow set of adjacent triangle-list opportunities; it neither merges draws nor establishes complete resource/state compatibility. Any real merging must preserve query boundaries, resource lifetimes, base vertices, device limits and output. [Draw-span predicate](../src/aotr_draw_spans.h).

Terrain also already has frustum-based visible bounds and partial/dirty update mechanisms. Avoid claiming a new camera or dirty cache before observing redundant rebuilds. Trees, tracks, shadows, shroud and water can have separate costs even with a stationary height map. [Visible terrain box](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/GameEngineDevice/Source/W3DDevice/GameClient/BaseHeightMapGetMaximumVisibleBox.cpp), [BFME1 partial terrain update](https://github.com/Open-BFME/Open-BFME-1/blob/d5dc2643a2420b720554c148a4868de4fa02532c/game/GameEngineDevice/Source/W3DDevice/GameClient/HeightMapRenderObjClass_doPartialUpdate.cpp).

**Anticipated gains:** 10–30% less exclusive submission work after excluding particle batching, sort and upload costs; with a 15% frame share this saves 1.5–4.5% frame time, or about 1.5–4.7% more FPS. For terrain, 5–20% less CPU preparation is a cautious target only if redundant work is found; at a 10% share that saves 0.5–2% frame time, or about 0.5–2% more FPS. Confidence is low for both ranges. Terrain tessellation and expensive shadows are also mentioned by the map guide, but that does not measure retail CPU submission versus GPU cost. [Map-design guidance](https://www.the3rdage.net/files/6700/BFME2_Map_Tips.pdf).

## 7. Script lookup and update overhead

GeneralsGameCode PR #3096 reported about **85% lower `ScriptEngine::findScript()` cost**, tested in release builds on the same AoD map. Its change returns references from script/group/name accessors instead of creating temporary `AsciiString` values. The author reports that eliminating copies helped more than removing the global string lock. This is a specific copying/reference-management lead, not an 85% gain for the entire script engine. [Upstream script lookup PR](https://github.com/TheSuperHackers/GeneralsGameCode/pull/3096).

The relevant BFME code needs to be mapped before a port. BFME2 name resolution supports qualified `(prefix, name)` keys, and team lookup can create or activate instances. Its unit lookup already tries a stored ID. Changing all named lookup APIs to one cached pointer would lose these semantics. [Name resolution](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/GameEngine/Source/GameLogic/ScriptEngine/ScriptEngineResolveName.cpp), [team lookup](https://github.com/Open-BFME/Open-BFME-2/blob/2786d73a13b2228753e49b937519925679556abc/Code/GameEngine/Source/GameLogic/ScriptEngine/ScriptEngineGetTeamNamed.cpp).

First count script-name comparisons, temporary string/reference operations, search length, misses and lookup calls per tick. If the copying pattern is confirmed, replace the verified comparison loop with direct access to stable names while preserving the original ABI and ownership. A source-level return-type change cannot simply be applied to a retail function whose callers expect a by-value result. A name index additionally needs first-match order, prefixes, renaming, load/reset and deletion coverage.

**Anticipated gain:** 50–85% less time in a lookup helper that actually exhibits the same temporary-copy pattern. At a 5% frame share, this saves 2.5–4.25% frame time, or about 2.6–4.4% more FPS. Confidence is medium in the precedent and low in RotWK applicability. At a 0.5% share even an 85% helper reduction saves only 0.425% of the frame. Script condition evaluation and side-effecting actions need separate measurement; they should remain ordered on the simulation thread.

## Combining gains without promising 120 FPS

For a CPU-bound serial critical path, let `f` be the fraction spent in a component and `r` the fraction of that component's time removed:

`new time / old time = 1 - f * r`

`FPS multiplier = 1 / (1 - f * r)`

The per-area examples above use independent hypothetical shares. They are not a measured profile and must not be added together. Particle batching overlaps submission and uploads; model work overlaps drawable updates; everything inside logic overlaps the logic timer. CPU work hidden behind GPU execution or another thread can shrink without changing displayed FPS.

For illustration, a deliberately disjoint 12.5 ms CPU-critical workload might comprise pathfinding 30%, model preparation 20%, particle preparation/submission 15%, buffers 10%, other submission 10%, script lookup 5% and other object work 10%. Applying reductions of 25%, 25%, 30%, 20%, 20%, 70% and 10% respectively removes **25.5%**. That changes **12.5 to 9.3125 ms**, approximately **80 to 107.4 FPS**. Every share and reduction here is assumed; none is a RotWK measurement. Even several successful changes can fall short of 120 FPS.

| Starting FPS, assuming CPU-bound sustained timing | Frame budget now | Time reduction required for 120 FPS |
| ---: | ---: | ---: |
| 100 | 10.000 ms | 16.7% |
| 90 | 11.111 ms | 25.0% |
| 80 | 12.500 ms | 33.3% |
| 60 | 16.667 ms | 50.0% |
| 30 | 33.333 ms | 75.0% |

The existing heavy one-core synthetic candidate already spends approximately **8.42 ms** on selected sort/audio/color routines, before real movement, animation and graphics. It is a constructed workload rather than a representative game frame, but directly rules out using those fixture gains as proof that every large battle fits 8.33 ms on one core. Average FPS also cannot establish a p95/p99 frame budget.

## What cores 2, 3 and 4 can realistically do

| Physical cores available | Suggested work allocation | Expected limitation |
| ---: | --- | --- |
| 1 | Keep all exact reductions useful in serial; all game/render work shares the core | A worker does not create more CPU capacity; wake/join and queue overhead still exist |
| 2 | Main handles ordered simulation and preparation; second core runs the existing D3D worker | Benefit depends on overlap and native lock/queue dependencies; current preparation pool deliberately adds no compute worker |
| 3 | Add one worker for stable particle/model numeric ranges, with main participating | Jobs must be large or aggregated; callbacks and shared scratch can erase the benefit |
| 4 | Add two preparation workers, reserving a core for rendering | Memory bandwidth, cache traffic, main participation and join latency limit scaling |

For an eligible preparation fraction `f`, ideal three-way execution changes that fraction to `f/3` before overhead. If 60% of the CPU critical path remains serial and 40% can run three-way, the ceiling is about **1.36x**, not 3x. Snapshot, dispatch, join and validation reduce it further. The current pool's paired 2.63x result concerns one large numeric pack, not the entire frame.

The best next offload is aggregated particle geometry or independently owned numeric model preparation with frozen inputs and private outputs. Use serial callbacks before/after the job boundary. The current pool is SSE-only and propagates MXCSR state; x87 engine arithmetic needs its own precision/control/status contract. Independent path searches are a later candidate because world/occupancy state, search scratch and ordered budget accounting are harder to isolate. General scripts, combat actions, RNG, object destruction and path-result commits should retain their original simulation order.

## Synthetic audit plan and priorities

Keep the supplied patched EXE fixed. First map and guard each new native routine, then construct a stock-versus-candidate fixture. Synthetic inputs are the primary next step; a live rendered check remains necessary for claims involving drivers, GPU work or full-game behavior.

| Priority | Synthetic workload and decisive measurements | Required equivalence |
| --- | --- | --- |
| 1 | Path rings/searches: open field, choke, blocked goals, moving obstacles; separate both lanes, request age, expansions, retries and zone work | Same paths/ties, request order, budget accounting, completion ticks and occupancy mutations |
| 1 | Small particle systems sharing or alternating texture/shader/frame state; live-versus-capacity sweeps; packed bytes, flushes and draws | Same ordered vertices/indices, tails, modes and callback order; rendered pixel comparison for actual batching |
| 1 | Script-name search: 32/256/2,048 entries, hits/misses, duplicates and qualified names; allocations/reference operations and search cost | Same first result, canonical output, lifetime and mutation behavior; native ABI unchanged |
| 2 | Stationary/moving/attacking drawable sets at equal counts; exclusive callback/query costs and allocation counts | Same callbacks, scheduling, cross-object results and destruction order |
| 2 | Repeated main/shadow model passes, equal/different poses, attachments and callback mutation; tree, deformation, packing and bounds separately | Same consumed bytes and floating-point state; no stale pose/bounds or reordered callbacks |
| 2 | Dynamic VB/IB harness: flags, small/large writes, read-modify-write, resets and overlapping lifetimes; native D3D9 and DXVK | Same contents, lock results, resource lifetime, command ordering and rendered output |
| 3 | Fixed/moving camera with terrain, trees, tracks, shroud/water and shadow variants; dirty rebuild counts and separate CPU/GPU time | Same visibility, dirty updates and pixels; no deferred simulation work |

Report one/two/three/four **physical-core** cases, one allowed logical CPU per selected core, with the whole process constrained. Alternate paired A/B order; distinguish cold proof windows from steady state. Retain regressions and driver/backend identity. The existing i7-1165G7 results provide a reference machine, not a universal scaling guarantee.

Current reporting has queued-Present interval distributions and inclusive logic/client/particle-manager/path timers. It lacks a verified exclusive breakdown for all seven areas. Inclusive timings overlap; they cannot be summed into a frame budget. Add low-overhead counters and properly nested exclusive measurements before choosing a broad rewrite. Track simulation ticks and request latency beside native Present completion or displayed intervals; queued submission alone is not displayed FPS.

## Binary checks and scope of this report

Fresh read-only checks of the supplied files retained these SHA-256 identities:

- DAT: `5481DE75B63E22483660EA8097478FAEF594F90B9ADE7DE893E092256BA68A26`.
- EXE: `B1D88FD1456BF9ECAA4BAE98E059C1BB6B0DE81E882B75BA4E80792277642D8B`.

| Existing guarded body | VA / bytes | DAT FNV-1a | EXE FNV-1a | Observation |
| --- | --- | --- | --- | --- |
| Logic wrapper | `006329B0` / 356 | `60BF8EBD` | `1698D50B` | Seven byte differences inside this body; both versions already guarded |
| Client wrapper | `00632409` / 294 | `4D60CC97` | `4D60CC97` | Identical |
| Particle-manager entry | `005F5123` / 327 | `0698EE30` | `0698EE30` | Identical; not an exclusive total of all FX work |
| Path queue | `006F2364` / 666 | `4F58A019` | `4F58A019` | Identical; two-ring static observations described above |

This audit inspected accelerator code, selected reference bodies/ledger claims, current upstream changes and native instructions. It reused the prior synthetic captures rather than generating new performance measurements. It did not run the game, install DXVK, alter settings, change the FPS patch or implement new hooks. New gain ranges are conditional forecasts; only results explicitly linked to the existing captures are measured locally. The report's calculations, links and whitespace were checked before committing.
