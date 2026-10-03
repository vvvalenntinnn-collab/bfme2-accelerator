// Real shipping buffer Lock/Unlock, recorder and executor; no driver required.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
static bool failPoolAllocation;
static LPVOID WINAPI testHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    return failPoolAllocation ? NULL : HeapAlloc(heap, flags, size);
}
#define HeapAlloc testHeapAlloc
#include "aotr_accel.cpp"
#undef HeapAlloc

struct Buffer {
    void** methods;
    DWORD refs, len, locks, unlocks, lastOff, lastSize, lastFlags;
    bool failLock;
    BYTE* bytes;
};
static HANDLE releaseEntered, releaseGate;
static bool gateRelease;
static DWORD __stdcall refBuffer(void* self) { return ++((Buffer*)self)->refs; }
static DWORD __stdcall releaseBuffer(void* self) {
    if (gateRelease) {
        SetEvent(releaseEntered);
        assert(WaitForSingleObject(releaseGate, 5000) == WAIT_OBJECT_0);
    }
    Buffer* b = (Buffer*)self; assert(b->refs); return --b->refs;
}
static DWORD __stdcall lockBuffer(void* self, DWORD off, DWORD size, DWORD out, DWORD flags) {
    Buffer* b = (Buffer*)self;
    assert(b->refs && off <= b->len && size <= b->len - off);
    ++b->locks; b->lastOff = off; b->lastSize = size; b->lastFlags = flags;
    if (b->failLock) return 0x8876086Cu;
    *(void**)out = b->bytes + off;
    return 0;
}
static DWORD __stdcall unlockBuffer(void* self) { ++((Buffer*)self)->unlocks; return 0; }
static DWORD __stdcall descBuffer(void* self, BYTE* desc) {
    memset(desc, 0, 32); *(DWORD*)(desc + 16) = ((Buffer*)self)->len; return 0;
}
static void* methods[14];
static void clearPool() {
    rtUploadReap();
    assert(!g_rtUploadFirst && !g_rtNStage);
    if (g_rtUploadHeap) assert(HeapDestroy(g_rtUploadHeap));
    g_rtUploadHeap = NULL; g_rtUploadBytes = 0;
    memset(g_rtUploadFree, 0, sizeof(g_rtUploadFree));
}
static void reset() {
    rtUploadReap(); assert(!g_rtUploadFirst && !g_rtNStage);
    g_rtPH = RtProdHot(); g_rtPB = RtPubHot(); g_rtWH = RtWorkHot();
    g_rtSeq = g_rtExecSeq = 7;
    g_rtArenaTop = 0; g_rtHaveResourceRelease = false;
    g_rtBMAny = 0; g_rtFreshN = 0; g_rtObjRefs = true; g_rtUploadOn = true;
    rtOpNoTouchInit();
}
static RtRec* queuedAt(DWORD head) {
    RtRec* r = (RtRec*)(g_rtBuf + head);
    if (r->kind == 2) r = (RtRec*)g_rtBuf;
    return r;
}
static RtRec* finish(Buffer& b, int op) {
    DWORD head = g_rtLocalHead;
    assert(rtBufUnlock(&b, methods, op) == 0);
    return queuedAt(head);
}
static void consume(RtRec* r) {
    rtExec(r); g_rtExecSeq = r->seq;
    g_rtTail = (DWORD)((BYTE*)r - g_rtBuf) + r->size;
}
static BYTE* start(Buffer& b, int op, DWORD off, DWORD size, DWORD flags) {
    BYTE* mapped = NULL;
    assert(rtBufLock(&b, methods, op - 1, off, size, (DWORD)&mapped, flags, 0) == 0);
    assert(mapped && !((ULONG_PTR)mapped & 15)); return mapped;
}
static void testCases(Buffer& b) {
    const DWORD sizes[] = { 1024, RT_UPLOAD_MIN, 65537 };
    const DWORD flags[] = { 0x1000, 0x2000 };
    for (int op = 342; op <= 362; op += 20)
        for (int sz = 0; sz < 3; ++sz) for (int f = 0; f < 2; ++f)
            for (int on = 0; on < 2; ++on) for (int refs = 0; refs < 2; ++refs) {
                reset(); b.refs = 1; b.locks = b.unlocks = 0;
                memset(b.bytes, 0xCC, b.len);
                g_rtUploadOn = on != 0; g_rtObjRefs = refs != 0;
                BYTE* mapped = start(b, op, 17, sizes[sz], flags[f]);
                memset(mapped, 0x36, sizes[sz]);
                RtRec* r = finish(b, op);
                bool leased = on && sz != 0;
                assert((rtArgs(r)[6] != 0) == leased);
                assert(b.refs == (refs ? 2u : 1u));
                assert(r->size == rtAl16(sizeof(RtRec) + 7 * 4) + (leased ? 0 : rtAl16(sizes[sz])));
                consume(r);
                for (DWORD i = 0; i < b.len; ++i) assert(b.bytes[i] == (i >= 17 && i < 17 + sizes[sz] ? 0x36 : 0xCC));
                assert(b.refs == 1 && b.locks == 1 && b.unlocks == 1);
                assert(b.lastOff == 17 && b.lastSize == sizes[sz] && b.lastFlags == flags[f]);
                rtUploadReap(); assert(!g_rtUploadFirst);
            }
}
static void testConcurrentLocks(Buffer& a, Buffer& b) {
    reset(); a.refs = b.refs = 1;
    BYTE* pa = start(a, 342, 0, RT_UPLOAD_MIN, 0x2000);
    BYTE* pb = start(b, 362, 0, RT_UPLOAD_MIN, 0x1000);
    assert(pa != pb && g_rtNStage == 2);
    memset(pa, 0x11, RT_UPLOAD_MIN); memset(pb, 0x22, RT_UPLOAD_MIN);
    RtRec* rb = finish(b, 362); RtRec* ra = finish(a, 342); // reverse Lock order
    assert(!g_rtNStage && g_rtUploadFirst == (RtUploadBlock*)rtArgs(rb)[6]);
    RtUploadBlock* extra = rtUploadAcquire(RT_UPLOAD_MIN);
    assert(extra && rtUploadData(extra) != pa && rtUploadData(extra) != pb);
    consume(rb); consume(ra);
    assert(a.bytes[0] == 0x11 && b.bytes[0] == 0x22 && a.refs == 1 && b.refs == 1);
    rtUploadQueue(extra); rtUploadDone(extra); rtUploadReap();
    assert(!g_rtUploadFirst);
}
static DWORD WINAPI executeOnThread(void* record) { rtExec((RtRec*)record); return 0; }
static void testLastAccess(Buffer& b) {
    reset(); clearPool(); b.refs = 1;
    BYTE* mapped = start(b, 342, 0, RT_UPLOAD_MIN, 0x2000);
    memset(mapped, 0x57, RT_UPLOAD_MIN);
    RtRec* r = finish(b, 342);
    RtUploadBlock* block = (RtUploadBlock*)rtArgs(r)[6];
    releaseEntered = CreateEventA(NULL, TRUE, FALSE, NULL);
    releaseGate = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(releaseEntered && releaseGate); gateRelease = true;
    HANDLE worker = CreateThread(NULL, 0, executeOnThread, r, 0, NULL);
    assert(worker && WaitForSingleObject(releaseEntered, 5000) == WAIT_OBJECT_0);
    assert(!InterlockedCompareExchange(&block->done, 0, 0));
    RtUploadBlock* other = rtUploadAcquire(RT_UPLOAD_MIN);
    assert(other && other != block); // not reusable while native Release is executing
    SetEvent(releaseGate);
    assert(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
    gateRelease = false;
    assert(block->done && b.refs == 1 && b.bytes[0] == 0x57);
    RtUploadBlock* reused = rtUploadAcquire(RT_UPLOAD_MIN);
    assert(reused == block && !reused->done);
    rtUploadQueue(other); rtUploadDone(other);
    rtUploadQueue(reused); rtUploadDone(reused); rtUploadReap();
    CloseHandle(worker); CloseHandle(releaseEntered); CloseHandle(releaseGate);
}
static void testFallbacks(Buffer& b) {
    reset(); clearPool(); b.refs = 1;
    RtUploadBlock* held[4];
    for (int i = 0; i < 4; ++i) { held[i] = rtUploadAcquire(RT_UPLOAD_MAX); assert(held[i]); }
    assert(g_rtUploadBytes == RT_UPLOAD_LIMIT && !rtUploadAcquire(RT_UPLOAD_MIN));
    memset(start(b, 342, 0, RT_UPLOAD_MIN, 0x2000), 0x81, RT_UPLOAD_MIN);
    RtRec* copied = finish(b, 342); assert(!rtArgs(copied)[6]); consume(copied);
    assert(b.bytes[0] == 0x81);
    for (int i = 0; i < 4; ++i) { rtUploadQueue(held[i]); rtUploadDone(held[i]); }
    rtUploadReap();
    RtUploadBlock* small = rtUploadAcquire(RT_UPLOAD_MIN); assert(small);
    assert(g_rtUploadBytes <= RT_UPLOAD_LIMIT); // cached large block trimmed to make space
    rtUploadQueue(small); rtUploadDone(small); rtUploadReap(); clearPool();
    failPoolAllocation = true;
    memset(start(b, 362, 0, RT_UPLOAD_MIN, 0x1000), 0x62, RT_UPLOAD_MIN);
    copied = finish(b, 362); assert(!rtArgs(copied)[6]); consume(copied);
    failPoolAllocation = false; assert(b.bytes[0] == 0x62); clearPool();

    b.failLock = true;
    memset(b.bytes, 0xA7, b.len);
    memset(start(b, 342, 0, RT_UPLOAD_MIN, 0x2000), 0xEE, RT_UPLOAD_MIN);
    RtRec* failed = finish(b, 342); consume(failed); rtUploadReap();
    assert(!g_rtUploadFirst && b.refs == 1 && b.bytes[0] == 0xA7);
    b.failLock = false;
}
static void testPlainMirror(Buffer& b) {
    reset(); b.refs = 2;
    RtBufMir& mirror = g_rtBM[0]; memset(&mirror, 0, sizeof(mirror));
    mirror.mem = (BYTE*)VirtualAlloc(NULL, b.len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(mirror.mem); memset(mirror.mem, 0x43, b.len);
    mirror.buf = (DWORD)&b; mirror.len = mirror.cap = b.len; mirror.live = 1;
    mirror.lastFrame = g_mkFrames; g_rtBMAny = 1; rtBMIndexRebuild();
    BYTE* mapped = start(b, 342, 9, RT_UPLOAD_MIN, 0);
    for (DWORD i = 0; i < RT_UPLOAD_MIN; ++i) assert(mapped[i] == 0x43);
    memset(mapped, 0x95, RT_UPLOAD_MIN);
    RtRec* r = finish(b, 342); assert(rtArgs(r)[6]);
    assert(mirror.mem[9] == 0x95); consume(r); rtUploadReap();
    assert(b.bytes[9] == 0x95 && b.refs == 2 && b.lastFlags == 0);
    VirtualFree(mirror.mem, 0, MEM_RELEASE); mirror.live = 0; g_rtBMAny = 0;
    --b.refs;
}
static void testCapture(Buffer& b) {
    reset(); b.refs = 1;
    g_capBuf = (char*)VirtualAlloc(NULL, CAP_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_capBuf); g_capPos = 0; g_capOn = 1;
    memset(start(b, 342, 0, RT_UPLOAD_MIN, 0x2000), 0x61, RT_UPLOAD_MIN);
    RtRec* r = finish(b, 342); g_capOn = 0;
    assert(rtArgs(r)[6] && strstr(g_capBuf, "pl=16384 h=") && strstr(g_capBuf, "61616161"));
    consume(r); rtUploadReap(); VirtualFree(g_capBuf, 0, MEM_RELEASE); g_capBuf = NULL;
}
static double sample(Buffer& b, DWORD size, bool lease, DWORD count) {
    reset(); b.refs = 1; g_rtUploadOn = lease;
    LARGE_INTEGER frequency, begin, end; QueryPerformanceFrequency(&frequency);
    // Warm allocation/reuse before timing.
    memset(start(b, 342, 0, size, 0x2000), 0x23, size); consume(finish(b, 342));
    QueryPerformanceCounter(&begin);
    for (DWORD i = 0; i < count; ++i) {
        memset(start(b, 342, 0, size, 0x2000), (BYTE)i, size);
        consume(finish(b, 342));
    }
    QueryPerformanceCounter(&end); rtUploadReap();
    return (double)(end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / count;
}
int main(int argc, char**) {
    static_assert(sizeof(void*) == 4, "Build with the x86 toolset");
    methods[1] = (void*)refBuffer; methods[2] = (void*)releaseBuffer;
    methods[11] = (void*)lockBuffer; methods[12] = (void*)unlockBuffer; methods[13] = (void*)descBuffer;
    g_rtBuf = (BYTE*)VirtualAlloc(NULL, RT_BUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_rtArena = (BYTE*)VirtualAlloc(NULL, RT_ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    InitializeCriticalSection(&g_rtExecCs);
    Buffer a = {}, b = {}; a.methods = b.methods = methods; a.len = b.len = RT_UPLOAD_MAX + 64;
    a.bytes = (BYTE*)VirtualAlloc(NULL, a.len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    b.bytes = (BYTE*)VirtualAlloc(NULL, b.len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_rtBuf && g_rtArena && a.bytes && b.bytes);
    testCases(a); testConcurrentLocks(a,b); testLastAccess(a);
    testFallbacks(a); testPlainMirror(a); testCapture(a);
    puts("PASS: retained uploads, bytes/flags/order, delayed Release, bounded reuse, allocation/budget fallback, failed Lock, plain mirrors and capture");
    if (argc > 1) {
        DWORD processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
            SetThreadAffinityMask(GetCurrentThread(), processMask & (0u - processMask));
        const DWORD sizes[] = { 16384, 65536, 262144, 1048576 };
        for (int s = 0; s < 4; ++s) {
            double before[7], after[7]; DWORD count = (16u << 20) / sizes[s]; if (count < 64) count = 64;
            for (int rep = 0; rep < 7; ++rep) {
                if (rep & 1) { after[rep] = sample(a, sizes[s], true, count); before[rep] = sample(a, sizes[s], false, count); }
                else { before[rep] = sample(a, sizes[s], false, count); after[rep] = sample(a, sizes[s], true, count); }
            }
            std::sort(before, before+7); std::sort(after, after+7);
            printf("MOCK CYCLE %u bytes: copied %.2f ns; retained %.2f ns; ratio %.2fx\n", sizes[s], before[3], after[3], before[3]/after[3]);
        }
    }
    clearPool();
    VirtualFree(a.bytes, 0, MEM_RELEASE); VirtualFree(b.bytes, 0, MEM_RELEASE);
    VirtualFree(g_rtArena, 0, MEM_RELEASE); g_rtArena = NULL;
    VirtualFree(g_rtBuf, 0, MEM_RELEASE); g_rtBuf = NULL;
    DeleteCriticalSection(&g_rtExecCs);
    return 0;
}
