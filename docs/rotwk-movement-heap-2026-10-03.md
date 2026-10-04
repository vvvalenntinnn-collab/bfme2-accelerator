# RotWK movement: exact pathfinding heap optimization

The next change after [findScript](rotwk-script-lookup-2026-10-03.md) replaces two small pathfinding heap operations. It preserves native heap layouts, equal-cost choices and search expansion order. The final one-core capture shows **1.017–1.233x faster heap workloads**, approximately **2–19% less helper time**. The controlled choke-search driver improved **1.016x**. These are modest synthetic gains, not measured game FPS.

The remaining list is movement/pathfinding queries and congestion; particle/FX batching; model/animation preparation; repeated object/drawable work; remaining buffer/upload costs; and renderer/terrain work. This commit addresses one part of the first item. It does not complete movement optimization.

## Native evidence and implementation

Open-BFME-1's recovered `PathfindCellInfo_putOnSortedList.cpp` describes an intrusive sorted list. The supplied RotWK uses a binary min-heap instead. Replacing a presumed linear list would target the wrong algorithm. Open-BFME-1's `PathfindCell_costSoFar.cpp` and record/pool sources helped identify the native cost chain; native instructions confirmed RotWK's different cell-info layout and full DWORD coordinate reads. Open-BFME-2's recovered path/node sources supplied additional call/layout context but did not establish a matched complete RotWK search implementation. Both reference trees were read without changes.

In both supplied inputs, `006ECF1D` sifts a value upward and `006ECF65` performs Floyd's heap adjustment: descend through the selected children, then sift the saved value upward. The key is the unsigned WORD at cell-info `+0x10`, reached through the cell's first pointer. Equal child costs select the right child; upward sifting stops when the parent cost is less than or equal to the saved value. Keeping those choices matters: a valid heap with different ties can change the path search's expansion order.

Direct callers are `006F06CB` for push and `006F0737` for adjustment, with adjustment itself calling push at `006ECFB9`. The push wrapper derives its nonnegative hole index from a pointer-range count and uses top index zero. The adjustment body keeps its original hole as the upward limit. The replacement retains the same valid nonnegative index contract.

| Runtime guard | VA | Bytes | FNV-1a, EXE and DAT |
| --- | --- | ---: | --- |
| Heap push | `006ECF1D` | 72 | `71BF798C` |
| Heap adjustment | `006ECF65` | 96 | `BA8CDDB2` |

The replacement removes repeated signed-halving correction for positive parent indices, loads the saved value's priority once during upward sifting, and folds the upward phase into adjustment. Explicit x86 register allocation retains predictable conditional branches and avoids compiler spills and dependent conditional moves that slowed small heaps in the initial C++ version. Callee-saved registers, stack cleanup and the native incidental EAX value are preserved. No floating-point instructions are introduced.

There is no persistent priority or route cache. The heap array and cell-info fields are read again on each call. Keys follow the ordinary heap contract: they remain stable during a single operation. Live key changes between calls are supported, and differential tests also cover arrays that have lost their heap ordering. No callbacks, reference changes, RNG, allocation or shared scratch occur inside these two helpers.

The existing EXE/DAT identity gate and both full-body hashes must match. Before patching, a startup self-test compares native and candidate arrays after every operation across 72 workloads: twelve sizes from 1 to 256 and six priority patterns, including ties and WORD extremes. Trivial null-value cases are also checked. Both six-byte detours commit as one suspended-thread transaction; failed writes roll back both attempted entries. `AOTR_MOVEHEAP=0` opts out. A guard or startup mismatch leaves native operations installed.

These pure helpers use startup validation rather than per-call shadow copying or thread checks. Per-call checks erased the benefit of the rejected cost prototype, and copying a whole heap during steady-state verification would add linear work to a logarithmic operation. The implementation introduces no new worker job; separate, independently owned heaps can use the same stateless helper concurrently.

## Measurements and rejected candidates

Machine: Intel Core i7-1165G7. MSVC x86 `/O2 /MT`, production fixture, entire process pinned to logical CPU 4. Seven alternating A/B samples were used. Each heap sample builds and drains the same seeded heap 1,000 times, including its array copy. Each grid sample runs 128 searches, including driver allocation and tracing.

The fixture restores original entry bytes before each native timed sample and reinstalls the hooks before each candidate sample. Code edits and instruction-cache flushing occur outside the timer. Thus the timed native baseline has no trampoline overhead. Correctness tests use a private relocated native adjustment body whose push call targets the untouched push trampoline, so their oracle also executes both fully native operations.

| Workload | Native | Hooked | Speedup |
| --- | ---: | ---: | ---: |
| 32 entries, push and drain | 0.56 us | 0.45 us | 1.233x |
| 256 entries, push and drain | 5.77 us | 5.01 us | 1.151x |
| 2,048 entries, push and drain | 200.00 us | 196.63 us | 1.017x |
| Controlled choke search | 59.47 us | 58.51 us | 1.016x |

Times are rounded independently; ratios are calculated before rounding. The large-heap and driver results are near the measurement noise floor and should not be treated as dependable universal gains. Other development captures varied with CPU frequency/cache/branch behavior; the committed final capture is the result reported here. [EXE build, checks and timing](benchmarks/rotwk-movement-exe-2026-10-03.log), [DAT checks](benchmarks/rotwk-movement-dat-2026-10-03.log), [CSV](benchmarks/rotwk-movement-2026-10-03.csv).

The movement cost prototype at `00934602` returned matching arithmetic results but produced inconsistent timing gains, with turning cases sometimes slower. Adding thread checks and live shadow validation made it clearly slower. It is not shipped. The initial C++ heap prototype also regressed small heaps; its replacement was selected for the final implementation after native comparison. Existing cost/heuristic calculations, collision checks, neighbor order, queue lanes, expansion accounting, repath frequency, AI updates and the FPS patch are unchanged.

For a hypothetical 5% CPU critical-path fraction spent in these heap operations, the measured helper range implies roughly **0.1–1% more CPU-limited FPS**. That fraction is not measured. Expensive collision/passability checks, zones, formation updates, targeting and repeated repaths can dominate movement regardless of this saving. A 120 FPS dense-battle result requires larger reductions elsewhere and an actual frame profile.

## Validation and limits

Reporting and production movement fixtures pass against both EXE and DAT. Each configuration compares **108,000 native/hooked heap operations**, including simultaneous calls from two threads using private heaps. Cases cover 1–2,048 entries, repeated IDs, equal/extreme priorities, non-root holes and upward limits, between-call key mutation, trivial null values, complete array contents, read-only cell/info storage and floating-point control state. Native EAX results are checked for the trivial cases as well.

Each configuration also checks 160 controlled grid requests. The ordinary sweep produces **26,628 identical ordered expansions and 104,787 identical queried costs**, with matching routes, parent links and final accumulated costs. Cases include open fields, a choke, a full barrier, random clutter, and a gate that opens between requests. Both sides use the actual native movement cost helper; the driver supplies modeled occupancy, neighbor generation and an integer heuristic. It does not execute retail collision/formation/zone logic, the request queue or the full search. The game was not launched and no game file was changed on disk.

Both complete DLL configurations build successfully. The existing RotWK work and script fixtures were rebuilt and passed in both configurations. The production DLL is x86, 807,936 bytes, SHA-256 `C5D65B7BC6246CB9996411C8E28DE372EE3D5CCB5948DE07F7843006C56A1569`. The supplied EXE/DAT identity hashes remain those recorded in the [bottleneck audit](rotwk-bottleneck-gain-audit-2026-10-03.md).

The next movement targets are exclusive measurements of path requests and blocked/repeated work, passability/filter queries, zone refresh and horde collision/formation work. They offer more scope than another tiny arithmetic helper. Offloading whole movement updates remains premature because occupancy, RNG and result commits are shared and ordered. Private search scratch or batched immutable queries are more plausible boundaries once their inputs and accounting are established.

## Reproduce

From the repository root in PowerShell, with `pefile`, `capstone` and x86 MSVC available:

```powershell
$env:AOTR_VCVARS32 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
py src/prepare_particle_retail_test.py --retail 'C:\RotWK\lotrbfme2ep1.exe' --output "$env:TEMP\rotwk-move-exe.bin" --movement
src/build_rotwk_movement_test.bat "$env:TEMP\rotwk-move-exe.bin" benchmark
py src/prepare_particle_retail_test.py --retail 'C:\Users\vvval\Downloads\game (1)\game.dat' --output "$env:TEMP\rotwk-move-dat.bin" --movement
src/rotwk_movement_report_test.exe "$env:TEMP\rotwk-move-dat.bin"
src/rotwk_movement_prod_test.exe "$env:TEMP\rotwk-move-dat.bin"
Remove-Item -LiteralPath "$env:TEMP\rotwk-move-exe.bin", "$env:TEMP\rotwk-move-dat.bin"
```

Only private temporary mapped bytes and in-process native oracles are used. Game bytes and generated DLLs are not committed. Runtime: [aotr_rotwk_movement.inc](../src/aotr_rotwk_movement.inc). Fixture: [rotwk_movement_test.cpp](../src/rotwk_movement_test.cpp).
