# Staged buffers and texture/surface dependency tracking

Improvement 7, prepared after `719672a` (grouped indexed mesh-picking bounds).

Staged vertex- and index-buffer uploads were recorded through `rtObj`, which stamped the two shared resource-dependency tables and checked the exact-resource and fresh-texture registries. Those tables answer whether a texture/surface can be locked or read without waiting for queued work. A buffer upload cannot access those resources, but its pointer could collide with an unrelated resource in both shared tables and make the first query about that resource return pending.

The existing `g_rtOpNoTouch` classification now includes `vb.Unlock` (342) and `ib.Unlock` (362), alongside the existing vertex-input/shader operations. These staged records name only the buffer being uploaded. They skip dependency-table stamps and fresh-texture lookup while keeping the rest of `rtObj` intact, including conditional AddRef and the record's release mask.

## Why this preserves synchronization

- `rtRefPending` is queried by texture/surface locks and the UpdateSurface source-read path. Vertex/index-buffer locks use staging/mirrors or an unconditional queue drain; they do not query these tables.
- A staged upload still copies its bytes into the queue at record time. The executor performs the original Lock, byte copy, Unlock and conditional Release in queue order, using the same offsets, sizes and lock flags.
- Texture/surface naming and pending checks keep their existing tracking. This includes device/effect SetTexture, texture/surface Unlock, and the containing texture checked by surface locks. The 192-entry exact index, collision policy and eviction behavior are unchanged.
- Buffer/resource Release operations retain their existing classification. Pending-resource-release flags and the device-release barrier remain intact.
- A non-staged buffer Unlock still queues the original single call.

## Potential impact

Every staged VB/IB upload avoids the two shared sequence-table writes, the diagnostic operation-table write, and the associated exact/fresh registry checks. It also removes this source of conservative false-pending results for unrelated textures/surfaces. Such a false result can force a queue wait where the resource itself has no pending work.

The benefit depends on staged-upload volume, resource-query frequency, collisions, and whether a wait lies on the frame's limiting path. Dynamic geometry uploads, including particle geometry where it uses this staging path, may benefit. This change does not accelerate particle simulation or reduce transparent-particle overdraw. No live-game FPS gain or frequency of these collisions was measured; the expected routine saving is small, with potentially larger savings when an unnecessary wait is avoided.

## Validation

Run from an x86 Native Tools prompt, or set `AOTR_VCVARS32` to the installed `vcvars32.bat`:

```bat
cd src
build_buffer_tracking_test.bat
```

The fixture includes the shipping `aotr_accel.cpp` and uses COM-shaped mock buffers; it does not install hooks or require a GPU. Both reporting and production variants passed:

- 72 upload cases spanning VB/IB, plain/NOOVERWRITE/DISCARD flags, previous/current classification, reference retention enabled/disabled, caller-reference removal where retention is enabled, and sequence wraparound.
- A constructed two-hash collision reproduces the previous false-pending result for a previously unqueried resource. The current classification leaves that unrelated resource idle.
- 32 genuinely pending-resource cases cover device/effect SetTexture and texture/surface Unlock classifications, with both early exact-entry creation and first queries after an upload, including sequence wraparound. They remain pending until their own queued call completes.
- Reused staging memory, overlapping uploads, exact copied bytes, offsets, sizes, flags, AddRef/Release lifetime, existing release markers and non-staged Unlock behavior are checked.

The existing real-worker render-queue tests passed in both modes, covering ordering, ring/sequence wraps, waits, message pumping, scopes, backpressure and watchdog behavior. The device-release regression fixture passed. Full x86 reporting and production DLL builds succeeded with freshly compiled rpmalloc.

These checks validate queue/tracker behavior with fixtures. Actual driver output and gameplay frame times still need an in-game comparison.

## Publishing

Commit `src/aotr_rt.inc`, `src/buffer_tracking_test.cpp`, `src/build_buffer_tracking_test.bat`, this document and the README update together. The destination is `vvvalenntinnn-collab/bfme2-accelerator`. This workspace does not permit Git metadata writes, so use the user's terminal to commit and push.
