# Remove report-only render-queue overhead in production

This follows audit item A8 and is separate from the allocator/preshader change prepared as PR 4. Production still timed every render-worker batch, message-aware wait, Present-backpressure interval, and scoped D3DX helper call despite lacking the reporting thread.

Use the existing `PSTAT` switch and a report-only clock-declaration macro to omit these clocks and related report counters under `AOTR_PROD`. Omit worker CPU-location reporting as well; explicit `AOTR_V4_CPUSTAT` experiment counters remain available. Drain completion no longer gathers report-only elapsed time.

The drain's start clock and long-wait warning checks remain active. Message-aware waits still execute and pump sent messages, worker sleeps retain their two-millisecond timeout, queue progress/ownership and sent-message handling are unchanged, and Present backpressure still waits for outstanding Presents. D3DX scopes retain the execution lock, direct-scope nesting, original call/result, and thread-depth tracking. This changes measurement overhead, not batching policy or draw ordering.

Potential impact is lower CPU overhead in queue-heavy workloads. No battle FPS measurement is available; this does not reduce particle simulation or GPU pixel work. Other unconditional reporting work remains outside this change.

## Validation

Run `src/build_render_queue_overhead_test.bat` from an x86 Native Tools prompt, or set `AOTR_VCVARS32` to the x86 toolchain initialization script. The script compiles rpmalloc freshly and runs both reporting and production test executables.

The test includes the shipping implementation and exercises:

- Signaled and timed-out waits, plus cross-thread `SendMessage` through an invisible message window. Sent-message pumping remains functional with report clocks absent.
- Present backpressure and the no-wait path.
- A scoped D3DX call with nesting/lock state checks and preservation of a failure HRESULT.
- A real render worker processing 10,000 ordered records, plus a gated record; ring and sequence-number wrap.
- The long-drain warning using an accelerated fixture clock, followed by successful queue completion. Production retains watchdog clocks while recording no worker/report timings.
- Production report counters remaining untouched and reporting mode retaining its expected clock calls.

Both test modes passed. Full reporting and production x86 DLL builds passed with fresh rpmalloc. These tests use synthetic queued calls; native D3D9/DXVK and live-game validation remain separate work.

## Pull request destination

Target **`vvvalenntinnn-collab/bfme2-accelerator`**, the user's fork. Intended branch: `optimization/render-queue-overhead-pr5`. Use `optimization/production-hotpath-pr4` as the stacked base, or `main` if that branch's tip has merged. The number in the branch name denotes change order; GitHub assigns actual PR numbers.

This session cannot write the workspace's `.git` or connect to GitHub. The implementation, tests, standalone patch, and a publishing script for PR 3, PR 4, and this change are prepared locally. The script explicitly pushes to the fork and uses the fork as the PR base repository.
