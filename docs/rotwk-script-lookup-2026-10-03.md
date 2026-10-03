# RotWK findScript: first implementation and measured gain

The accelerator now fuses RotWK's indexed `ScriptList` name search and result lookup. The supplied FPS-patched `lotrbfme2ep1.exe` and reference `game.dat` have identical code for this capability. The steady-state synthetic lookup is **1.19–1.81x faster than the previous accelerator**, including the hook's native verification on one call in 64. This is approximately **16–45% less time in this helper**. It is not a measured FPS increase.

## What the binary actually does

The earlier [bottleneck audit](rotwk-bottleneck-gain-audit-2026-10-03.md) used the upstream SAGE `findScript` improvement as a reason to investigate name lookup. That upstream change removes string copies from a linear scan. RotWK already has a sorted index and compares stored strings without copying them in the search loop. Its binary does not support applying that upstream 85% helper saving directly.

`Open-BFME-1` provided the named `ScriptEngine::findScript` wrapper and its call chain. `Open-BFME-2` provided the corresponding indexed implementation and storage layout. The relevant BFME2 reference files are under `Code/GameEngine/Source/GameLogic/ScriptEngine/`: `BfmeOwnZC_rva00204F3B.cpp`, `ScriptListRva003B6911.cpp`, and `ScriptListSubrecordRemove.cpp`. Their opaque recovered names were treated as layout/call-chain evidence, not as additional named-symbol proof. Reference revisions were `d5dc2643a2420b720554c148a4868de4fa02532c` and `2786d73a13b2228753e49b937519925679556abc`, respectively. Native disassembly of both user binaries independently confirmed the mapped bodies below.

The native wrapper resolves the prefix/side, selects a `ScriptList`, searches its second table, optionally assigns the resolved prefix to the canonical output, and destroys its temporary and by-value input strings. Inside the list, binary search returns either an equality midpoint or an insertion position. Another wrapper repeats an equality comparison before fetching the record's node payload at `nodes + 4`.

The new implementation keeps that outer wrapper native and replaces only the 41-byte list lookup entry. It fuses the search, equality decision, and payload fetch. It also uses the accelerator's existing length-bounded SSE2 `fastMemcmp`, avoiding layers of native comparison helpers. The baseline benchmark already uses that same fast imported comparator, so the measured gain is additional to the existing fast-CRT optimization.

| Guarded native body | VA | Bytes | FNV-1a, both inputs |
| --- | --- | ---: | --- |
| Qualified findScript wrapper, unchanged | `00604A5D` | 128 | `D338F3DD` |
| ScriptList lookup, detoured | `007B72EC` | 41 | `CFD191AD` |
| Index lookup and repeated equality | `007B712D` | 67 | `A4310078` |
| Midpoint binary search | `007B6287` | 80 | `D4DE2712` |
| StringBase comparison | `004065AA` | 42 | `2486D6F6` |
| Length-aware comparison wrapper | `00406307` | 63 | `08FD6040` |
| Bounded byte comparison | `004052F9` | 43 | `3FE1424B` |

Installation requires the existing eligible EXE/DAT identity, all seven body hashes, and the verified five-byte detour boundary. It uses the existing suspended-thread transaction and rollback implementation. A changed dependency leaves this capability off. `AOTR_SCRIPTLOOKUP=0` disables installation. No engine file is changed on disk.

## Behavior preserved

The second table is at list offset `+0x2C`. Its sorted array stores record IDs, not contiguous records; the record array starts at list `+0x38` and has a 20-byte stride. The replacement follows those IDs and retains the native binary search's equality midpoint. It does not substitute lower-bound semantics when duplicate names exist.

String buffers carry a 16-bit length at `+4` and bytes at `+8`. Comparison uses `memcmp` over the minimum length, followed by the length difference. Embedded NULs participate, high bytes compare unsigned, and null/allocated empty buffers compare equal. The returned address retains native wrapping `nodes + 4`, including the unusual null-node result of address 4.

There is no persistent name or pointer cache. Each call reads the current index, names, and node heads. Insertions, removals, renames, record-slot reuse, and node-head changes therefore require no additional invalidation hooks. Lookup does not change string ownership, reference counts, scheduling, or simulation order.

Only calls on the identified game/render main thread use the candidate. Calls before that thread is identified, calls from other threads, and uncertain vector layouts use native lookup. The first 512 eligible calls execute both implementations and return the native result. Thereafter, one call in 64 does the same. A differing result disables the candidate for the session and returns the native result. Startup and proof/mismatch messages are logged; the hook adds no per-call timing or report thread.

## Synthetic measurements

Machine: Intel Core i7-1165G7. The entire fixture process was constrained to logical CPU 0 for the benchmark. Production code was compiled with MSVC x86 `/O2 /MT`. Each result is the median of seven alternating A/B samples, 200,000 calls per sample. Inputs use 50% hits and 50% misses, split between names below and above the sorted range. Matching query buffers are independent copies. Names are 15 bytes normally, or have an additional 128-byte common prefix. The shipping 1-in-64 verification remains enabled during timing; the cold 512-call proof window is completed first.

| Scripts | Shared prefix bytes | Previous accelerator, ns/call | New hook, ns/call | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0 | 26.82 | 14.84 | 1.81x |
| 1 | 128 | 52.62 | 34.35 | 1.53x |
| 2 | 0 | 40.39 | 22.81 | 1.77x |
| 2 | 128 | 61.05 | 36.79 | 1.66x |
| 8 | 0 | 63.35 | 40.15 | 1.58x |
| 8 | 128 | 101.70 | 67.84 | 1.50x |
| 32 | 0 | 92.82 | 68.97 | 1.35x |
| 32 | 128 | 190.40 | 142.93 | 1.33x |
| 256 | 0 | 198.53 | 145.31 | 1.37x |
| 256 | 128 | 297.08 | 221.87 | 1.34x |
| 2,048 | 0 | 259.16 | 216.99 | 1.19x |
| 2,048 | 128 | 421.52 | 324.75 | 1.30x |

Raw [EXE output](benchmarks/rotwk-script-exe-2026-10-03.log), [DAT checks](benchmarks/rotwk-script-dat-2026-10-03.log), and [CSV](benchmarks/rotwk-script-2026-10-03.csv) are committed. These are warm in-memory searches, not whole-script execution, side-name resolution, displayed frames, or a late-battle replay. Frequency/cache variation affects absolute times. The comparison replaces only the import needed by this isolated native call chain; the benchmark import points to the same fast comparator used by the existing accelerator.

An initial candidate using plain C `memcmp` regressed on large common-prefix tables. Reusing `fastMemcmp` removed that regression. The final twelve cases all improved. That initial candidate is not shipped.

## Expected effect on the 120 FPS target and extra cores

For a measured baseline helper fraction `f` and helper speedup `s`, the ideal CPU critical-path speedup is `1 / (1 - f + f/s)`. Using the measured range, a 1% helper fraction implies roughly 0.2–0.5% more CPU-limited FPS; 5% implies roughly 0.8–2.3%; 10% implies roughly 1.7–4.7%. These are conditional estimates. Native prefix/side resolution and script execution are outside the optimized helper, and a GPU limit can absorb the CPU saving entirely. No full-game helper fraction has been measured here.

At 10,000 comparable calls per frame, the captured per-call differences imply roughly 0.12–0.97 ms less lookup work. The actual call count and script-name mix are unknown. This change alone does not establish a dense-battle 8.33 ms frame budget or guarantee 120 FPS.

This helper stays on the main thread. A 15–325 ns lookup is too small to justify worker dispatch/join overhead, and the outer script operations have ordered ownership/world effects. Cores 2–4 remain better candidates for the [audit's aggregated numeric particle/model preparation](rotwk-bottleneck-gain-audit-2026-10-03.md#what-cores-2-3-and-4-can-realistically-do), with frozen inputs and private outputs. This commit adds no new offload or change to logic tick frequency.

## Validation and reproduction

Both report and production fixtures passed against both supplied inputs. Each fixture performs 7,730 differential comparisons with the host CRT and another 7,730 with the game's actual `msvcr71.dll` comparator, for 61,840 checks across the four configurations. Cases cover empty/single/large tables; sparse/reordered IDs; duplicate midpoint selection; all hits and between-name misses in unique tables; live insert/remove/rename/reuse and node mutations; null/empty/embedded-NUL/high-byte/65,535-byte names; page-end comparison bounds; and unchanged record/string contents and floating-point control state.

Tests also cover every dependency hash, failure after an actual detour write and exact rollback, opt-out, the 512-call proof transition and sampling count, unknown/foreign-thread and uncertain-layout fallback, and mismatch shutdown. The unchanged native outer wrapper runs with controlled prefix/side/string dependencies to verify its stack ABI, selected/missing side, hit/miss output writes, optional canonical output and destructor order, with both native and candidate lookup. Those controlled dependencies do not validate the full engine's prefix parser or allocator; those implementations are left untouched. The game was not launched.

Both complete DLL builds passed. The existing RotWK work fixture was also rebuilt and run in both configurations to check integration with render-list sorting, audio, numeric preparation and reporting. The current production DLL is x86, 805,888 bytes, SHA-256 `B9DBBC3F057FE022EDA0BD05B506A6F4BDD9F90339901EC74EAE89B954EB804D`; generated DLLs remain build artifacts rather than committed binaries.

Input SHA-256 values remain:

- EXE: `B1D88FD1456BF9ECAA4BAE98E059C1BB6B0DE81E882B75BA4E80792277642D8B`.
- DAT: `5481DE75B63E22483660EA8097478FAEF594F90B9ADE7DE893E092256BA68A26`.
- Native test CRT: `8094AF5EE310714CAEBCCAEEE7769FFB08048503BA478B879EDFEF5F1A24FEFE`.

Run from the repository root in PowerShell, with `pefile`, `capstone`, and an x86 MSVC toolchain available:

```powershell
$env:AOTR_VCVARS32 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
$env:AOTR_TEST_MSVCR71 = 'C:\RotWK\msvcr71.dll'
py src/prepare_particle_retail_test.py --retail 'C:\RotWK\lotrbfme2ep1.exe' --output "$env:TEMP\rotwk-script-exe.bin" --script
src/build_rotwk_script_test.bat "$env:TEMP\rotwk-script-exe.bin" benchmark
py src/prepare_particle_retail_test.py --retail 'C:\Users\vvval\Downloads\game (1)\game.dat' --output "$env:TEMP\rotwk-script-dat.bin" --script
src/rotwk_script_report_test.exe "$env:TEMP\rotwk-script-dat.bin"
src/rotwk_script_prod_test.exe "$env:TEMP\rotwk-script-dat.bin"
Remove-Item -LiteralPath "$env:TEMP\rotwk-script-exe.bin", "$env:TEMP\rotwk-script-dat.bin"
```

`AOTR_TEST_MSVCR71` is fixture-only; omit it for the host-comparator checks alone. Private mapped native bytes are generated in Temp from the user's files and are not committed. Runtime implementation: [aotr_rotwk_script.inc](../src/aotr_rotwk_script.inc). Tests: [rotwk_script_test.cpp](../src/rotwk_script_test.cpp).
