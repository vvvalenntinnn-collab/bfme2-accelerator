# Faster indexed mesh bounds

Follow-up to audit item A11. The mesh-picking pretest gathers bounds from every indexed triangle vertex before deciding whether the stock ray cast can be skipped. This is extra work even when the ray remains a possible hit.

The bounds loop now handles four indexed vertices per iteration, combines their NaN checks, and uses a balanced min/max reduction before updating the running bounds. The remaining vertices use the original single-vertex loop. This shortens the running min/max dependency and reduces loop/check branches.

It still reads exactly three floats from each referenced vertex, including current deformed geometry, and uses the same index order for equal-value ties. There is no geometry cache, extra allocation, altered ray margin, or new executable address. The existing proof phase, sampled stock checks, mismatch shutdown, build gates, and kill switch remain active. Both verified BFME2 and RotWK mesh hooks use this helper.

## Validation

Run from an x86 Native Tools prompt, or set `AOTR_VCVARS32` to your x86 toolchain initialization script:

```bat
src\build_pick_bounds_test.bat benchmark
```

The test includes the shipping implementation and compares it with the frozen pre-change bounds implementation from `28b0908`. It covers 2,000 random geometry cases, every loop-tail size, repeated indices, the maximum 16-bit index, missing buffers, signed-zero tie patterns, infinities/NaNs at every position in small fixtures, and a guard page immediately following the last vertex. Successful bounds match bit for bit; failure results and untouched output buffers match as well.

Synthetic medians from seven samples per mesh size, alternating before/after order and using fixed CPU affinity on this machine:

| Triangles | Previous bounds | Grouped bounds | Throughput ratio |
| --- | --- | --- | --- |
| 12 | 41.01 ns | 35.26 ns | 1.16x |
| 64 | 213.20 ns | 172.98 ns | 1.23x |
| 256 | 841.19 ns | 697.29 ns | 1.21x |
| 1,024 | 3,437.60 ns | 2,763.01 ns | 1.24x |
| 4,096 | 13,127.05 ns | 10,532.79 ns | 1.25x |

These are hot-buffer bounds timings with synthetic index distributions. They exclude stock collision testing, scene traversal, rendering, and GPU work. The helper took about 14–20% less time in these samples; this is not a whole-game FPS percentage. For example, 1,000 casts of the tested 1,024-triangle fixture would save about 0.67 ms of bounds work. Actual battle cast counts and geometry were not measured.

The differential test, existing BFME2 hook tests including retail signature/layout checks, and full reporting/production x86 DLL builds passed. No live-game or driver comparison was performed.

## What changes 1–5 can add up to

| Change | Evidence | Where the game may benefit |
| --- | --- | --- |
| 1. Independent BFME2 equivalence memo and picking pretest | Stock-proof and fallback fixtures; no gameplay timing | Repeated template comparisons and rays missing complex meshes; benefit depends on hit rates and proof activation |
| 2. Device Release fast path | Fixture removes 64 eligible barriers before its cap forces one | More overlap between the game and render worker when temporary device releases previously drained queued work |
| 3. Resource hash index | Earlier synthetic lookup: 52.80 to 6.44 ns, about 8.2x lookup throughput | Frequent exact resource tracking; 10,000 comparable lookups would save about 0.46 ms of lookup work |
| 4. Allocator/preshader reporting cleanup | First synthetic run: 18.98 to 12.59 ns per allocation/free pair; 32.33 to 28.60 ns per one-double cached preshader call | Many small allocations or cached shader calls; 10,000 of each would save about 0.10 ms using those measurements |
| 5. Render-queue reporting cleanup | Production report clocks absent in real-worker fixtures; watchdog and ordering preserved | Busy queues, frequent waits/scopes; no measured frame-time reduction yet |

These mechanisms overlap, affect different builds/workloads, and may execute outside the frame's limiting path. Do not add their helper speedup percentages together. A GPU-limited scene may see little FPS benefit, while a CPU scene with expensive avoidable barriers or stock casts can benefit more. None of these changes offloads particle simulation or reduces transparent-particle pixel work.

Overall FPS requires a fixed battle/replay comparison with the same camera, settings, backend, and build. Measure median and p95/p99 frame times as well as average FPS. As a scale example, saving 1 ms from a 20 ms CPU-limited frame would change 50 FPS to about 52.6 FPS; saving 5 ms would change it to about 66.7 FPS. Those are arithmetic examples, not measured or predicted gains from the accelerator.

## Publishing

This is the next separate change after the five local commits, prepared against `28b0908`. Commit the helper, test, build script, this document, and README update together. The destination remains the user's fork, `vvvalenntinnn-collab/bfme2-accelerator`. Git metadata writes and GitHub connectivity remain unavailable to this session, so publishing requires the user's terminal.
