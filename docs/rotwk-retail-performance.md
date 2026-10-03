# RotWK retail particles and staged uploads

These changes build on the earlier resource hash index, production reporting reductions, device-release
barrier reduction, indexed bounds and buffer dependency tracking improvements.

The enabled additions retain large staged uploads, fuse particle color packing/clamping, and index
positive particle-system lookups. A per-call HLod matrix cache was implemented and checked, but is
**disabled by default**: it did not consistently outperform the stock routine.

The [synthetic implementation follow-up](rotwk-synthetic-implementation-2026-10-03.md) adds guarded
retail sort/push/erase, audio indexing, adaptive ID rebuilds, physical-core placement and coarse
profiling. Joined particle workers and selected-particle gathering are experimental and off by default.

## Supported executable and switches

The engine additions support two independently verified inputs at image base `00400000`:

- `C:\Users\vvval\Downloads\game (1)\game.dat`: complete `.text` FNV-1a `4404F3C6`,
  image size `00AD3000`, checksum `00ADC2F6`.
- The inspected `C:\RotWK\lotrbfme2ep1.exe`: complete `.text` FNV-1a `88C193EE`,
  image size `00AD4000`, checksum `00AE0F79`.

Both have timestamp `460DA09E` and entry RVA `0063D082`. The direct executable differs in 176
code bytes, but all nine particle/pose/handle routine bodies checked by the fixture are identical.
Native reporting and production differential tests passed separately using this direct executable.
The DLL independently applies its complete code hash and routine body checks.

Matching header metadata alone is insufficient. Both remain in the limited-capability build
category; it does not inherit the older `5ED63115` build's complete address table or pose workers.
Future RotWK/2.02/Age of the Ring executables with different code receive portable acceleration only
until their engine routines are independently checked.

| Switch | Default | Scope |
| --- | --- | --- |
| `AOTR_UPLOADLEASE=0` | Enabled | Portable large staged VB/IB upload retention |
| `AOTR_PARTICLECOLOR=0` | Enabled on the exact supported executable | Fused full-capacity color packing |
| `AOTR_PARTICLEINDEX=0` | Enabled on the exact supported executable | Positive ID lookup index |
| `AOTR_POSEMATRIX=1` | **Disabled** | Explicitly opt into the experimental HLod matrix cache |
| `AOTR_RLSORT=0` | Enabled on verified targets | Disable sort/push/erase installation |
| `AOTR_RLPUSH=0` | Enabled | Keep push/erase native while retaining sorting |
| `AOTR_AUDIOLIMIT=0` | Enabled on verified targets | Disable audio indexing |
| `AOTR_PARTICLEWORKERS=1` | **Disabled** | Joined preparation for eligible large particle packs |
| `AOTR_PARTICLEGATHER=1` | **Disabled** | Experimental fused selected-particle gather |
| `AOTR_WORKERPLACEMENT=0` | Enabled | Disable physical-core render-worker placement hints |

Color and ID acceleration wait for the render recorder to identify the game thread. Unidentified or
foreign-thread lookups use the original routines. Color/pose use stock code with unmasked SSE exceptions.

## Retained upload storage

For eligible 16 KiB through 4 MiB staged writes, `Lock` returns aligned storage whose lifetime extends
through the worker's native `Lock`, copy, `Unlock`, and conditional buffer `Release`. `Unlock` queues
the storage pointer rather than copying its payload into the queue. The worker publishes completion
after its last access; only the producer allocates, reclaims or frees storage.

Blocks are linked in unlock/execution order, which can differ from lock order. The retained pool has
a 16 MiB payload budget, plus headers and heap metadata, in addition to the existing staging arena.
Completed blocks are reused by size class or trimmed under budget pressure. In-flight/locked blocks
are never recycled. Allocation failure, budget exhaustion, small/oversized writes, and disabled retention
use the existing copied path. Native upload errors, object references, pending-resource checks,
plain-lock mirror contents and queue ordering follow the existing behavior. Capture records still hash
the actual upload bytes.

This removes one large CPU copy; it does not remove the native driver upload or create parallel game simulation.

## Particle color packing

The original routine copies RGB and opacity into an RGBA buffer and then clamps all capacity slots.
The replacement combines these operations into one SSE pass, retaining missing-channel defaults,
NaN payloads, signed zeros and masked floating-point flags. It reads exactly three RGB floats per
particle and writes every capacity slot, including unused tails needed by other render consumers.
It does not reduce particle counts or alter lifetimes, emission, visibility, RNG or simulation updates.

Stock code retains allocation and default-only buffer release. Unsupported capacities (above 65,536),
missing storage and overlapping input/output buffers use stock code. The first 512 eligible calls
compare complete replacement output with stock output while returning stock output. Afterwards one
call in 64 is checked. Any byte mismatch disables the fused pass for the session. Checks remain in production.

## Positive particle-system ID lookup

The legacy manager walks its ordered list of weak handles. The replacement builds a bounded index
after eight queries without intervening mutation, or 32 after a recent short burst, for lists of at least 32 nodes. Longer quiet windows restore the eight-query policy. The 8,192-slot
tables use bounded linear probing; excessive collisions or more than 4,096 nodes leave lookup stock.
Only successful IDs are indexed; misses and ID zero always use the original walk. Duplicate IDs retain
the first list node, exactly as the walk does. Returned handles are constructed by the original
weak-handle copy constructor, preserving observer registration and unlink behavior.

Node insertion, erase (including null handles), clear, assignment into tracked handles, and the
save/load `ParticleSystemID` helper invalidate the index. Reentrant calls during a mutation stay stock.
Hits validate the current handle and ID before use; null observer writes caused by system destruction
force stock fallback and rebuild. Foreign-thread mutation disables the index for the session.
The first 512 indexed hits, then one in 64, return stock results and verify the selected system.
Mismatches disable the index. Memory is approximately 96 KiB plus a small header.

All six lookup/lifetime detours must be prepared together. Complete function bodies are checked again
while other threads are suspended. Installation aborts if a thread cannot be suspended/inspected or
has its instruction pointer inside a stolen prologue. Partial patch failures restore earlier bytes
and leave indexing disabled; original trampolines remain available for forwarding.

## Pose experiment

The prototype retains the native animation-base update and every child callback in retail order,
while caching quaternion-to-matrix conversion inside a single HLod update. Input bytes, pivot pointer,
MXCSR controls and sticky exception flags participate in reuse checks. Child callbacks still run on
the calling thread; pivot data is reloaded after callbacks. Additional-model translation retains the
retail arithmetic order. Nothing is cached across frames.

Native comparisons passed, including callbacks that change pivots, replace the tree, change rounding
and clear floating-point flags. However, cache validation and copying cost enough that timings ranged
from near parity with a shared pivot to a regression with varied pivots. Leave the prototype off for
normal use. Further work needs a cheaper reuse strategy and a measured benefit on real model layouts.

## Routine measurements

Production fixture, x86 MSVC `/O2`, fixed thread affinity, seven samples with alternating run order;
values below are medians from one run. Color inputs include varying values outside the clamp range;
these results depend strongly on data and branch prediction. Live 1-in-64 verification is included.

| Workload | Stock/copied | Accelerated/retained | Routine speedup |
| --- | ---: | ---: | ---: |
| Particle color, capacity 192 | 820 ns | 278 ns | 2.95x |
| Particle color, capacity 4,096 | 72.1 us | 6.19 us | 11.65x |
| Particle color, capacity 65,536 | 2.14 ms | 154 us | 13.92x |
| ID hit, 64 systems | 62.1 ns | 11.0 ns | 5.64x |
| ID hit, 256 systems | 219 ns | 14.5 ns | 15.08x |
| ID hit, 512 systems | 463 ns | 19.4 ns | 23.81x |
| Mock upload cycle, 16 KiB | 709 ns | 208 ns | 3.41x |
| Mock upload cycle, 64 KiB | 3.64 us | 1.91 us | 1.90x |
| Mock upload cycle, 256 KiB | 15.7 us | 7.90 us | 1.99x |
| Mock upload cycle, 1 MiB | 108 us | 55.1 us | 1.96x |

Upload timings include producer writes and a mock native upload cycle, with the shipping recorder and
executor. They exclude a real graphics driver. ID timings include weak-handle creation and unlink.
The pose prototype measured 4.75 us stock versus 4.69 us cached for 256 children sharing a bone,
and 4.40 us versus 5.59 us for 64 varied bones across those children; these measurements informed
the default-off decision.

**These are routine measurements, not combined gameplay FPS measurements.** From the prior work,
a rough unmeasured expectation for all additions on top of the base accelerator is **5–15% shorter
heavy CPU frame times when the affected paths consume enough of the frame**. A 40 ms frame would
become approximately 34–38 ms (25 FPS to about 26–29 FPS). Gains can be much smaller in frames
dominated by untouched simulation, pathfinding, other rendering work or the GPU. Do not add helper
speedup percentages together. Measure the same replay/camera with the same executable and mod,
using the base accelerator and this version, to establish average and slow-frame gains.

## Validation

The fixture runs retail routines in a private suspended child process
with mock game objects/callbacks; it never starts the game. Animation-base work and allocator entry
points are replaced only in that private mapping. Actual Age of the Ring battles, rendered frames,
multiplayer and driver behavior still need in-game validation.

Reporting and production modes passed:

- 480 native color cases across channel combinations, capacities 0–65,536, NaNs, infinities, signed
  zeros, denormals, rounding/FTZ/DAZ settings, guard regions, live proofs and conservative exits.
- 600 native HLod cases comparing matrix bytes, callback sequence/arguments, dirty flags and MXCSR,
  including exceptional offsets, repeated/different bones and callback mutations/tree replacement.
- Native lookup and observer links, duplicates, misses, collisions, capacity exhaustion, insertion, erase/null erase, weak observer
  nulling, clear, reassignment, save/load reentrancy, small lists and foreign-thread fallback.
- Deliberate color/ID proof mismatches return stock output and disable their fast paths.
- Failed installation at each of six patch steps, restoring all original body hashes; rejection of
  a modified body before patching; environment disable switches and pose default-off behavior.
- Retained upload bytes/flags/order, multiple locks unlocked in reverse order, delayed native release,
  allocation and budget fallback, native lock failure, mirror prefill/writeback and capture.
- Existing buffer tracking and production hot-path regressions.

Prepare an ephemeral reference (requires Python `pefile` and `capstone`), then run from PowerShell:

```powershell
$env:AOTR_VCVARS32 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
$reference = Join-Path $env:TEMP 'rotwk-fx-reference.bin'
python src/prepare_particle_retail_test.py --retail 'C:\Users\vvval\Downloads\game (1)\game.dat' --output $reference
if ($LASTEXITCODE -ne 0) { throw 'Retail reference validation failed' }
& .\src\build_rotwk_fx_test.bat $reference benchmark
& .\src\build_upload_lease_test.bat benchmark
& .\src\build_buffer_tracking_test.bat
& .\src\build_production_hotpath_test.bat
& .\src\build_prod.bat
```

The reference contains proprietary executable sections, is generated in Temp, and must not be committed
or distributed. The Python preparation verifies the complete code hash, nine complete routine hashes
and eight detour instruction boundaries. All new implementation is independently authored.

To commit this batch to your fork from the repository root:

```powershell
git add README.md docs/rotwk-retail-performance.md src/
git diff --cached --stat
git commit -m "Optimize RotWK particle colors, ID lookups and staged uploads"
git push origin HEAD:main
```

`origin` should be `https://github.com/vvvalenntinnn-collab/bfme2-accelerator.git` (check `git remote -v`).
The two older untracked audit documents are separate from this batch. Review the staged list before committing.
