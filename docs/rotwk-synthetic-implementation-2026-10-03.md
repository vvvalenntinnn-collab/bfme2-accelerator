# ROTWK accelerator: implementation and synthetic results

The new default capabilities are retail render-list sorting/push/erase, an audio request index, adaptive particle ID rebuilds and physical-core-aware render-worker placement. Reporting builds now measure the supplied EXE's coarse logic, client, particle-manager and path-queue routines. A joined preparation pool and a selected-particle gather prototype are available for experiments.

This follows the [late-game audit](rotwk-late-game-audit-2026-10-03.md). The audit describes the earlier snapshot; this document records the resulting implementation. The game and FPS patch remain unchanged on disk. Both supplied binaries pass the new guards. Distribution binaries have not been replaced.

## Results that affect the defaults

The tests map sections from the user's own binary into a private fixture process. They execute actual stock routines beside the accelerator, with constructed lists, particle arrays and callback objects. They do not launch the game. The recorded machine is an Intel i7-1165G7, four physical cores and eight logical CPUs.

| Workload | Stock | Candidate | Decision |
| --- | ---: | ---: | --- |
| 2,500-entry render-list sort, production fixture | 763.835 µs | 28.345 µs | Enable guarded port; 26.95× in this fixture |
| One-core color pack, 4,096 slots, valid RGB/alpha | 16.942 µs | 5.038 µs | Retain fused serial packing; 3.36× |
| One-core audio count, 4,096 requests / 64 sound identities | 13.203 µs | 1.843 µs | Enable guarded index; 7.16× |
| One-core audio count, 512 requests / one sound identity | 1.139 µs | 1.258 µs | Use plain walk for dense buckets; wrapper still has overhead |
| One-core selected-particle gather, 65,536 selected entries | 455.858 µs | 636.323 µs | Keep fused gather disabled; 0.72× |
| HLod, 256 fake children / 64 different bones, later capture | 5.966 µs | 7.523 µs | Keep matrix cache disabled; 0.79× |

These are seven-sample medians of repeated operations, with baseline/candidate order alternated. Proof sampling remains active in the hooked sort, audio, ID and gather paths after their initial proof windows. The direct packing/pool comparisons isolate packing costs. The model fixture uses fake child callbacks; it does not measure a full army's animation or bounds work.

Raw captures and the [82-row CSV](benchmarks/rotwk-synthetic-2026-10-03.csv) include regressions. Use the `workload` and `build` columns: rows named “joined pool versus serial SIMD” have the serial accelerator helper as their baseline, rather than game machine code. Absolute times vary with turbo, scheduling and background activity; compare the paired timings within each experiment.

### Extra cores

Each core-matrix fixture constrains the entire child process to one logical CPU on each selected physical core. Main participates in preparation; one core is reserved for rendering, although these CPU fixtures do not execute a D3D/GPU workload. The pool uses zero workers with one/two available cores, one worker with three, and two with four.

| Allowed physical cores | Preparation workers | 65,536-color pack versus stock | Joined pack versus serial SIMD, separately paired |
| ---: | ---: | ---: | ---: |
| 1 | 0 | 3.22× | 1.01×; serial fallback |
| 2 | 0 | 2.86× | 1.01×; serial fallback |
| 3 | 1 | 5.82× | 1.77× |
| 4 | 2 | 8.16× | 2.63× |

At the 32,768-slot dispatch threshold, joined packing beat serial SIMD by 1.54× with one worker and 2.24× with two. Ordinary 192/4,096-slot systems stay serial. A 65,536-entry scattered gather improved only 1.42× over stock with two workers, while small/serial gathers regressed. Extra cores do not make every fused loop worthwhile.

### Dense combined CPU work

The combined fixture processes one render list, one complete audio-count pass and 64 separate 4,096-slot color arrays. The color storage totals 8 MiB across independent input/output ranges. This exposes work growth without relying on a replay.

| Physical cores | 2,500 meshes / 512 requests: stock → candidate | 10,000 meshes / 4,096 requests: stock → candidate |
| ---: | ---: | ---: |
| 1 | 2.534 → 0.481 ms | 47.792 → 8.419 ms |
| 2 | 3.691 → 0.693 ms | 40.879 → 5.663 ms |
| 3 | 2.599 → 0.507 ms | 61.606 → 8.505 ms |
| 4 | 2.531 → 0.491 ms | 41.095 → 5.950 ms |

All color systems in this combined case are below the worker threshold. Its gains come from doing less serial work. The core-count rows do not establish whole-game scaling; CPU frequency varies, there is no native render workload, and adding idle allowed CPUs does not parallelize sorting or audio.

The heavy one-core candidate already takes about 8.42 ms for these selected routines. There is additional simulation, movement, animation, rendering and GPU work in a real game. These results establish useful acceleration and identify remaining costs; they do not establish sustained 120 FPS or a one-core late-battle guarantee.

### Particle ID churn

Steady positive lookups remain useful: the later capture measured 15.28× for 256 systems and 24.02× for 512 systems. The new churn fixture removes/reinserts a real native weak-handle list node, then performs a specified number of lookups, validating every returned system and unlinking observers.

| Lookups per erase/insert, 256 systems | Before: stock/candidate | Adaptive rebuild: stock/candidate |
| ---: | ---: | ---: |
| 1 | 0.93× | 0.99× |
| 8 | 0.49× | 0.85× |
| 32 | 1.70× | 1.80× |
| 128 | 5.04× | 4.58× |

An index built on the last lookup of an eight-query burst costs more than it saves. After a short observed burst, rebuilding now waits for 32 queries; a longer window restores the eight-query policy. The short-burst case still has forwarding/tracking overhead, but avoids repeated table clearing/building. Snapshot contents, first-duplicate selection, observer construction and lifetime invalidation are unchanged. No misses are cached.

## What was implemented

[aotr_rotwk_work.inc](../src/aotr_rotwk_work.inc) installs capabilities independently of the legacy address map. The render-list port checks its comparator/sort/heap/push/erase region and reference-release helper. Audio checks the manager region, all 35 mutation call sites and bounded list-primitive regions. Its 37 edits commit together while other threads are suspended and inspected. Failure restores every attempted edit, including an edit whose writer reports failure after modifying bytes.

Sorting retains stock comparator order, ties and heap fallback. Push retains list selection/reference counts; erase retains destruction order. Startup and live proofs return native results until the proof window completes; later samples detect mismatches and disable replacements. Foreign/unidentified render-thread calls remain native.

The audio index retains live candidate/flag checks, mutation tracking, rebuild limits and stock-answer verification. Buckets covering at least a quarter of the list, and null sound-info queries, now walk directly. This avoids treating a dense bucket as a useful index and preserves native null-info answers. Event-info immutability during indexed passes remains the existing algorithm's assumption; its deliberately illegal-mutation test detects a mismatch and disables indexing.

[aotr_prepare_pool.h](../src/aotr_prepare_pool.h) implements a joined, single-producer pool with bounded worker count, chunk claims, completion events, main participation, overlap rejection and shutdown/restart. Borrowed arrays remain valid until all workers finish. Current jobs use SSE, propagate MXCSR controls and combine sticky exception flags with the caller. Unmasked exceptions stay serial. The pool is explicitly restricted to SSE jobs; x87 work requires additional state handling before using it.

[aotr_cpu_topology.h](../src/aotr_cpu_topology.h) counts allowed physical cores rather than dividing logical CPU count. Soft placement retains main/render core roles before selecting preparation workers. It does not force process affinity in the game. The current 32-bit implementation conservatively uses processor group 0. Unknown topology creates no compute workers.

[aotr_rotwk_gather.inc](../src/aotr_rotwk_gather.inc) maps the reference APT helper to ROTWK `0057EA80`, verifies the helper and its four indexed-copy dependencies, and gathers location/diffuse/size/orientation/frame in one selected-index pass. It preserves index order and duplicates. Native resizing, default/no-APT branches, small batches, aliases and unsupported capacities remain native. Full-output proofs protect the prototype. Its measured serial regression keeps it off by default.

Reporting now accepts the FPS-patched logic body independently: `006329B0`, 356 bytes, DAT digest `60BF8EBD`, EXE digest `1698D50B`. The detour steals eight complete instruction bytes. Client, particle-manager and path-queue timers have separate full-body guards. Unsupported legacy sampling remains disabled. Foreign calls do not relabel the identified main thread's phase.

Queued-Present reporting includes the fraction over 8.333 ms, maximum and histogram p95/p99 bounds: 0.25 ms bins below 16 ms, 2 ms bins through 48 ms, then an overflow bin. These measure submission intervals while recording is active, not displayed FPS. Coarse phase times are inclusive and overlap when nested.

Reporting also counts adjacent triangle-list draw-range candidates within uninterrupted worker batches. It changes no draws. Device caps, instancing, resource lifetime and pixel/order equivalence must be established before turning those opportunities into native draw merges. Diagnostic clocks and these observers are excluded from production hot paths.

## Switches and use

| Switch | Default | Behavior |
| --- | --- | --- |
| `AOTR_RLSORT=0` | On for verified targets | Disable retail sort/push/erase installation |
| `AOTR_RLPUSH=0` | On | Keep push/erase native while retaining sorting |
| `AOTR_AUDIOLIMIT=0` | On for verified targets | Keep audio request counting native |
| `AOTR_WORKERPLACEMENT=0` | On | Disable the render worker's topology placement hints |
| `AOTR_PARTICLEWORKERS=1` or `PARTICLE_WORKERS_ON` | Off | Experimental joined large-pack preparation |
| `AOTR_PARTICLEGATHER=1` or `PARTICLE_GATHER_ON` | Off | Experimental selected-particle fused gather |
| `AOTR_POSEMATRIX=1` | Off | Existing model matrix-cache prototype |
| `AOTR_DIAG=1`, `DIAG_ON`, or `AOTR_PROFILE=1` | Off | Reporting build: guarded coarse timers; no unsupported legacy sampler |

Marker files go beside the DLL/log, so experimental switches remain usable through the elevated launcher. An explicit worker/gather environment value takes precedence over its marker. Remove experimental markers for the default configuration. Production builds have no coarse/profile report hooks.

## Remaining model, movement and batching work

The [Open-BFME-2 point-group reference](../../../Open-BFME-2/Code/Libraries/Source/WWVegas/WW3D2/PointGroupClassRender.cpp) led to the new native gather mapping. It also exposes shared compression/vertex arrays. Broader particle preparation needs private output ranges and a mapped pass boundary that can aggregate many small systems without changing their simulation/render order. The current pool joins inside an individual pack/gather call; it does not parallelize emission, creation or deletion.

Native HLod tests preserve matrices, flags, MXCSR and the order of callbacks that can mutate pivots or switch trees. Blanket model offload would invalidate that contract. The next model boundary must establish tree/subobject ownership, freeze numeric inputs, preserve serial callback order and handle required floating-point state. Cache and worker dispatch costs need to beat stock before enabling them.

The [Open-BFME-1 path-queue reference](../../../Open-BFME-1/game/GameEngine/Source/GameLogic/Pathfinder/PathfinderProcessPathfindQueue.cpp) retains ordered requests, shared zone updates and expansion budgets. The new ROTWK timer measures the queue; movement and path processing retain stock behavior. Independent search workers require private search scratch, a world snapshot, dependency validation in request order and recomputation of stale/dependent results. Serial result commits alone are insufficient. Exact query/index/allocation improvements remain preferable before changing simulation scheduling.

Native particle draw merging and broader simulation/model workers are outstanding implementation stages. The current code provides measurements and a tested numeric worker boundary, rather than enabling unverified whole-engine parallelism.

## Validation and reproduction

Both EXE and DAT fixtures passed in reporting and production: 2,304 native audio-loop cases, 96 parallel color cases including partial chunks and MXCSR modes, 128 native gather cases, startup plus 3,020 hooked sorts, and all 37 partial audio-edit rollbacks. The existing FX fixture passed 480 color and 600 HLod cases, ID lifetime/observer cases, adaptive rebuild transitions and six rollback positions. Existing full sort/comparator/heap/push/erase comparisons and randomized audio mutation/illegal-info tests passed. Diagnostic identity and render-queue regression fixtures passed in both modes.

```powershell
$env:AOTR_VCVARS32 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
$reference = Join-Path $env:TEMP 'rotwk-private-reference.bin'
py src\prepare_particle_retail_test.py --retail 'C:\RotWK\lotrbfme2ep1.exe' --output $reference --work
& .\src\build_rotwk_work_test.bat $reference benchmark
& .\src\build_rotwk_fx_test.bat $reference benchmark
& .\src\build_new.bat
& .\src\build_prod.bat
Remove-Item -LiteralPath $reference
```

Check each command's exit code before continuing. Run performance fixtures separately to avoid competing benchmark processes. The generated reference contains private game sections and must stay outside the repository; the saved text captures/CSV contain only results. Full DLL builds and final validation are recorded with this implementation; live graphics, replay/multiplayer determinism and sustained FPS remain unmeasured by these CPU fixtures.
