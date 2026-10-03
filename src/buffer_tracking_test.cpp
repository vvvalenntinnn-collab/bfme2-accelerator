// Exercise the shipping recorder, dependency tracker and executor without a GPU.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "aotr_accel.cpp"

struct BufferFixture {
    void** vtable;
    DWORD refs, locks, unlocks, destroyed;
    DWORD offsets[2], sizes[2], flags[2];
    BYTE bytes[64];
};
static DWORD __stdcall addRef(DWORD self) {
    BufferFixture* b = (BufferFixture*)self;
    assert(b->refs && !b->destroyed);
    return ++b->refs;
}
static DWORD __stdcall release(DWORD self) {
    BufferFixture* b = (BufferFixture*)self;
    assert(b->refs && !b->destroyed);
    if (!--b->refs) ++b->destroyed;
    return b->refs;
}
static DWORD __stdcall lockBuffer(DWORD self, DWORD off, DWORD size, void** mapped, DWORD flags) {
    BufferFixture* b = (BufferFixture*)self;
    assert(b->refs && !b->destroyed && b->locks < 2 && off + size <= sizeof(b->bytes));
    DWORD n = b->locks++;
    b->offsets[n] = off; b->sizes[n] = size; b->flags[n] = flags;
    *mapped = b->bytes + off;
    return 0;
}
static DWORD __stdcall unlockBuffer(DWORD self) {
    BufferFixture* b = (BufferFixture*)self;
    assert(b->refs && !b->destroyed);
    ++b->unlocks;
    return 0;
}
static void* bufferMethods[14];

static void resetQueue(DWORD base) {
    g_rtPH = RtProdHot(); g_rtPB = RtPubHot(); g_rtWH = RtWorkHot();
    g_rtSeq = g_rtExecSeq = base;
    for (DWORD i = 0; i < RT_REFN; ++i) {
        // Represent completed references, including near sequence wraparound.
        g_rtRefSeq[i] = g_rtRefSeq2[i] = base;
        g_rtRefOp[i] = 0xFFFF;
    }
    memset(g_rtX, 0, sizeof(g_rtX));
    memset(g_rtXHead, 0, sizeof(g_rtXHead));
    memset(g_rtXNext, 0, sizeof(g_rtXNext));
    memset(g_rtXMark, 0, sizeof(g_rtXMark));
    memset(g_rtOpNoTouch, 0, sizeof(g_rtOpNoTouch));
    g_rtXN = 0; g_rtNStage = 0; g_rtArenaTop = 0; g_rtFreshN = 0;
    g_rtObjRefs = true;
    g_rtHaveResourceRelease = true; g_rtLastResourceRelease = base;
    rtOpNoTouchInit();
    assert(g_rtOpNoTouch[87] && g_rtOpNoTouch[92] && g_rtOpNoTouch[100]);
    assert(g_rtOpNoTouch[104] && g_rtOpNoTouch[107]);
    assert(g_rtOpNoTouch[342] && g_rtOpNoTouch[362]);
    assert(!g_rtOpNoTouch[65] && !g_rtOpNoTouch[172]); // device/effect SetTexture
    assert(!g_rtOpNoTouch[220] && !g_rtOpNoTouch[304]); // texture/surface Unlock
    assert(!g_rtOpNoTouch[332] && !g_rtOpNoTouch[352]); // resource Release
}

// Invert the second hash, then search its bucket for a first-hash collision.
// This identity is used only by the tracker; it is never dereferenced.
static DWORD collidingResource(DWORD buffer) {
    DWORD inverse = 1;
    for (int i = 0; i < 5; ++i) inverse *= 2 - 2654435761u * inverse;
    DWORD bucket = rtRefIdx2(buffer) << 18;
    for (DWORD low = 0; low < (1u << 18); ++low) {
        DWORD candidate = (bucket + low) * inverse;
        if (candidate && candidate != buffer && !(candidate & 3) && rtRefIdx(candidate) == rtRefIdx(buffer)) {
            assert(rtRefIdx2(candidate) == rtRefIdx2(buffer));
            return candidate;
        }
    }
    assert(false && "could not construct the two-hash collision fixture");
    return 0;
}
static RtRec* upload(BufferFixture& buffer, int op, BYTE* bytes, DWORD off, DWORD size, DWORD flags) {
    DWORD head = g_rtLocalHead;
    RtStage& stage = g_rtStage[g_rtNStage++];
    stage.buf = (DWORD)&buffer; stage.off = off; stage.size = size;
    stage.flags = flags; stage.mem = bytes; stage.mir = NULL;
    g_rtArenaTop = 123;
    assert(rtBufUnlock(&buffer, bufferMethods, op) == 0);
    assert(!g_rtNStage && !g_rtArenaTop);
    RtRec* record = (RtRec*)(g_rtBuf + head);
    assert(record->kind == 3 && record->op == op && record->objMask == (g_rtObjRefs ? 1 : 0));
    assert(!(record->flags & 2));
    return record;
}
static void consume(RtRec* record) {
    rtExec(record);
    g_rtExecSeq = record->seq;
    g_rtTail = (DWORD)((BYTE*)record - g_rtBuf) + record->size;
}

static void testUpload(int op, DWORD flags, bool previousPolicy, bool dropCaller, DWORD base, bool retain = true) {
    resetQueue(base);
    g_rtObjRefs = retain;
    if (previousPolicy) g_rtOpNoTouch[op] = 0;
    BufferFixture b = {};
    b.vtable = bufferMethods; b.refs = 1;
    memset(b.bytes, 0xAA, sizeof(b.bytes));
    BYTE staged[20], expected[64];
    memcpy(expected, b.bytes, sizeof(expected));
    for (DWORD i = 0; i < sizeof(staged); ++i) staged[i] = (BYTE)(i * 7 + op);
    memcpy(expected + 7, staged, sizeof(staged));
    RtRec* record = upload(b, op, staged, 7, sizeof(staged), flags);
    assert(b.refs == (retain ? 2u : 1u) && !b.locks && !b.unlocks);
    assert(g_rtHaveResourceRelease && g_rtLastResourceRelease == base);
    DWORD surface = collidingResource((DWORD)&b);
    // A buffer stamp formerly made an unrelated, previously unqueried surface pending.
    assert(rtRefPending(surface) == previousPolicy);
    assert(g_rtRefSeq[rtRefIdx((DWORD)&b)] == (previousPolicy ? record->seq : base));
    assert(g_rtRefSeq2[rtRefIdx2((DWORD)&b)] == (previousPolicy ? record->seq : base));
    memset(staged, 0xEE, sizeof(staged)); // caller may reuse staging memory immediately
    if (dropCaller) assert(release((DWORD)&b) == 1);
    consume(record);
    assert(!memcmp(b.bytes, expected, sizeof(expected)));
    assert(b.locks == 1 && b.unlocks == 1);
    assert(b.offsets[0] == 7 && b.sizes[0] == sizeof(staged) && b.flags[0] == flags);
    assert(b.refs == (dropCaller ? 0u : 1u) && b.destroyed == (dropCaller ? 1u : 0u));
    assert(!rtRefPending(surface));
    assert(g_rtHaveResourceRelease && g_rtLastResourceRelease == base);
}

static DWORD surfaceCalls;
static DWORD __stdcall nameSurface(DWORD surface) {
    assert(surface); ++surfaceCalls; return 0;
}
static void testGenuinePending(int op, DWORD surfaceOp, DWORD base, bool queriedEarly) {
    resetQueue(base);
    BufferFixture b = {}; b.vtable = bufferMethods; b.refs = 1;
    DWORD surface = collidingResource((DWORD)&b);
    RtRec* named = rtAlloc(1, 0);
    named->op = (WORD)surfaceOp; named->fn = (void*)nameSurface;
    g_rtObjRefs = false; // numeric surface identity, never a COM fixture
    rtArgs(named)[0] = rtObj(named, 0, surface);
    g_rtObjRefs = true;
    rtCommit(named, 0);
    // Check both an existing exact entry and a first query after the upload.
    if (queriedEarly) assert(rtRefPending(surface));
    BYTE staged[8] = { 1,2,3,4,5,6,7,8 };
    RtRec* record = upload(b, op, staged, 3, sizeof(staged), 0x1000);
    assert(rtRefPending(surface));
    LONG index = rtXFind(surface);
    assert(index >= 0 && g_rtX[index].seq == named->seq);
    consume(named);
    assert(!rtRefPending(surface));
    assert(rtSeqPending(record->seq) && !b.locks); // upload still queued
    consume(record);
    assert(!rtRefPending(surface) && b.refs == 1 && b.unlocks == 1);
}

static void testOrderedUploads(int op) {
    resetQueue(7);
    BufferFixture b = {}; b.vtable = bufferMethods; b.refs = 1;
    BYTE staged[20], expected[64] = {};
    memset(staged, 0x12, sizeof(staged)); memcpy(expected + 7, staged, sizeof(staged));
    RtRec* first = upload(b, op, staged, 7, sizeof(staged), 0x2000);
    memset(staged, 0x34, sizeof(staged)); memcpy(expected + 15, staged, sizeof(staged));
    RtRec* second = upload(b, op, staged, 15, sizeof(staged), 0x1000);
    memset(staged, 0xEF, sizeof(staged));
    assert(b.refs == 3 && !b.locks);
    consume(first); assert(b.refs == 2 && b.locks == 1);
    consume(second);
    assert(b.refs == 1 && b.locks == 2 && b.unlocks == 2);
    assert(b.flags[0] == 0x2000 && b.flags[1] == 0x1000);
    assert(!memcmp(b.bytes, expected, sizeof(expected)));

    resetQueue(7); // non-staged unlock keeps its existing single-call behavior
    b = BufferFixture(); b.vtable = bufferMethods; b.refs = 1;
    assert(rtBufUnlock(&b, bufferMethods, op) == 0);
    RtRec* plain = (RtRec*)g_rtBuf;
    assert(plain->kind == 1 && !plain->objMask && b.refs == 1);
    consume(plain);
    assert(!b.locks && b.unlocks == 1 && b.refs == 1);
}

int main() {
    bufferMethods[1] = (void*)addRef; bufferMethods[2] = (void*)release;
    bufferMethods[11] = (void*)lockBuffer; bufferMethods[12] = (void*)unlockBuffer;
    g_rtBuf = (BYTE*)VirtualAlloc(NULL, RT_BUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_rtBuf);
    const DWORD flags[] = { 0, 0x1000, 0x2000 }; // plain, NOOVERWRITE, DISCARD
    const DWORD bases[] = { 7, 0xFFFFFFFEu };
    const DWORD surfaceOps[] = { 65, 172, 220, 304 };
    for (int op = 342; op <= 362; op += 20) {
        for (int f = 0; f < 3; ++f) for (int old = 0; old < 2; ++old)
            for (int drop = 0; drop < 2; ++drop) for (int wrap = 0; wrap < 2; ++wrap)
                testUpload(op, flags[f], old != 0, drop != 0, bases[wrap]);
        for (int f = 0; f < 3; ++f) for (int old = 0; old < 2; ++old)
            for (int wrap = 0; wrap < 2; ++wrap)
                testUpload(op, flags[f], old != 0, false, bases[wrap], false);
        for (int s = 0; s < 4; ++s) for (int wrap = 0; wrap < 2; ++wrap)
            for (int early = 0; early < 2; ++early)
                testGenuinePending(op, surfaceOps[s], bases[wrap], early != 0);
        testOrderedUploads(op);
    }
    assert(surfaceCalls == 32);
    VirtualFree(g_rtBuf, 0, MEM_RELEASE); g_rtBuf = NULL;
    puts("PASS: VB/IB collision avoidance, genuine pending resources, upload bytes/order, COM lifetime, release markers, sequence wrap and fallback unlock");
    return 0;
}
