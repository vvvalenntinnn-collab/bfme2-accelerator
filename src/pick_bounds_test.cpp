// Differential test/benchmark of the shipping indexed bounds helper.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <algorithm>
#include <vector>
#include "aotr_accel.cpp"

// Frozen pre-change implementation from revision 28b0908.
static bool referenceBounds(const BYTE* model, LONG polys, float* mn, float* mx) {
    DWORD ih = *(DWORD*)(model + 0x2C), vh = *(DWORD*)(model + 0x30);
    if (!ih || !vh) return false;
    const WORD* idx = *(const WORD**)(ULONG_PTR)(ih + 0xC);
    const float* v = *(const float**)(ULONG_PTR)(vh + 0xC);
    if (!idx || !v) return false;
    __m128 lo = pbLoadVertex(v + 3 * idx[0]), hi = lo;
    if (_mm_movemask_ps(_mm_cmpunord_ps(lo, lo))) return false;
    for (LONG i = 1, n = polys * 3; i < n; ++i) {
        __m128 q = pbLoadVertex(v + 3 * idx[i]);
        if (_mm_movemask_ps(_mm_cmpunord_ps(q, q))) return false;
        lo = _mm_min_ps(lo, q); hi = _mm_max_ps(hi, q);
    }
    float a[4], b[4]; _mm_storeu_ps(a, lo); _mm_storeu_ps(b, hi);
    mn[0] = a[0]; mn[1] = a[1]; mn[2] = a[2]; mx[0] = b[0]; mx[1] = b[1]; mx[2] = b[2];
    return true;
}

struct Fixture {
    DWORD model[13], ih[4], vh[4];
    std::vector<WORD> indices;
    std::vector<float> vertices;
    Fixture(LONG polys, size_t count) : indices(polys * 3), vertices(count * 3) {
        memset(model, 0, sizeof(model)); memset(ih, 0, sizeof(ih)); memset(vh, 0, sizeof(vh));
        model[0x2C/4] = (DWORD)ih; model[0x30/4] = (DWORD)vh;
        ih[3] = (DWORD)&indices[0]; vh[3] = (DWORD)&vertices[0];
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = (WORD)(i % count);
    }
};

static DWORD randomState = 0x41935682;
static DWORD nextRandom() { randomState = randomState * 1664525u + 1013904223u; return randomState; }
static void compare(const BYTE* model, LONG polys) {
    float oldMin[3] = {}, oldMax[3] = {}, newMin[3] = {}, newMax[3] = {};
    bool oldOk = referenceBounds(model, polys, oldMin, oldMax);
    bool newOk = pbVertexBounds(model, polys, newMin, newMax);
    assert(oldOk == newOk);
    assert(!memcmp(oldMin, newMin, sizeof(oldMin)) && !memcmp(oldMax, newMax, sizeof(oldMax)));
}
static void testRandom() {
    for (int test = 0; test < 2000; ++test) {
        LONG polys = 1 + nextRandom() % 2000;
        size_t count = 1 + nextRandom() % 2000;
        Fixture fixture(polys, count);
        for (size_t i = 0; i < fixture.vertices.size(); ++i) {
            DWORD bits = nextRandom();
            if ((bits & 0x7F800000u) == 0x7F800000u) bits ^= 0x00800000u;
            memcpy(&fixture.vertices[i], &bits, 4);
        }
        for (size_t i = 0; i < fixture.indices.size(); ++i) fixture.indices[i] = (WORD)(nextRandom() % count);
        compare((BYTE*)fixture.model, polys);
    }
}
static void testSpecial() {
    // Equal-value tie order matters for the exact sign bit of zero.
    for (LONG polys = 1; polys <= 17; ++polys) {
        Fixture fixture(polys, polys * 3);
        for (DWORD pattern = 0; pattern < 64; ++pattern) {
            for (size_t i = 0; i < fixture.vertices.size(); ++i) {
                DWORD bits = ((pattern >> (i % 6)) & 1) ? 0x80000000u : 0;
                memcpy(&fixture.vertices[i], &bits, 4);
            }
            compare((BYTE*)fixture.model, polys);
        }
        // Put each exceptional value into each indexed position, including tails.
        const DWORD exceptional[] = {0x7f800000u, 0xff800000u, 0x7fc00001u, 0xffc00001u};
        for (size_t i = 0; i < fixture.vertices.size(); ++i) {
            for (unsigned k = 0; k < 4; ++k) {
                std::fill(fixture.vertices.begin(), fixture.vertices.end(), 1.0f);
                memcpy(&fixture.vertices[i], &exceptional[k], 4);
                compare((BYTE*)fixture.model, polys);
            }
        }
    }
    Fixture fixture(12, 65536);
    fixture.indices[35] = 65535;
    fixture.vertices[65535 * 3] = 99;
    compare((BYTE*)fixture.model, 12);
    fixture.ih[3] = 0; compare((BYTE*)fixture.model, 12);
    fixture.ih[3] = (DWORD)&fixture.indices[0]; fixture.vh[3] = 0; compare((BYTE*)fixture.model, 12);
    fixture.model[0x30/4] = 0; compare((BYTE*)fixture.model, 12);
}
static void testGuardPage() {
    SYSTEM_INFO info; GetSystemInfo(&info);
    BYTE* pages = (BYTE*)VirtualAlloc(NULL, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(pages);
    DWORD old; assert(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
    float* vertices = (float*)(pages + info.dwPageSize - 9 * sizeof(float));
    const float input[] = {-1,-2,-3, 4,5,6, 1,2,3};
    memcpy(vertices, input, sizeof(input));
    for (LONG polys = 1; polys <= 17; ++polys) {
        Fixture fixture(polys, 3); fixture.vh[3] = (DWORD)vertices;
        compare((BYTE*)fixture.model, polys);
    }
    VirtualFree(pages, 0, MEM_RELEASE);
}

typedef bool (*Bounds)(const BYTE*, LONG, float*, float*);
static volatile DWORD benchmarkSink;
static __declspec(noinline) double measure(Bounds fn, Fixture& fixture, LONG polys, int iterations) {
    LARGE_INTEGER a,b,f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < iterations; ++i) {
        float mn[3], mx[3]; assert(fn((BYTE*)fixture.model, polys, mn, mx));
        DWORD bits; memcpy(&bits, mn, 4); benchmarkSink ^= bits;
    }
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart-a.QuadPart)*1e9/(double)f.QuadPart/iterations;
}
static void benchmark() {
    SetThreadAffinityMask(GetCurrentThread(), 1);
    const LONG counts[] = {12,64,256,1024,4096};
    for (unsigned count = 0; count < 5; ++count) {
        LONG polys = counts[count]; Fixture fixture(polys, polys/2+3);
        for (size_t i = 0; i < fixture.vertices.size(); ++i) fixture.vertices[i] = (float)(nextRandom()%100000)/100.0f;
        for (size_t i = 0; i < fixture.indices.size(); ++i) fixture.indices[i] = (WORD)(nextRandom()%(fixture.vertices.size()/3));
        int iterations = 1000000 / polys;
        double before[7], after[7];
        measure(referenceBounds, fixture, polys, 100);
        measure(pbVertexBounds, fixture, polys, 100);
        for (unsigned sample = 0; sample < 7; ++sample) {
            // Alternate order to reduce systematic warmup/frequency bias.
            if (sample & 1) {
                after[sample] = measure(pbVertexBounds, fixture, polys, iterations);
                before[sample] = measure(referenceBounds, fixture, polys, iterations);
            } else {
                before[sample] = measure(referenceBounds, fixture, polys, iterations);
                after[sample] = measure(pbVertexBounds, fixture, polys, iterations);
            }
        }
        std::sort(before,before+7); std::sort(after,after+7);
        printf("%ld triangles: before %.2f ns, after %.2f ns, %.2fx\n", polys, before[3], after[3], before[3]/after[3]);
    }
}
int main(int argc, char**) {
    static_assert(sizeof(void*) == 4, "Use the x86 MSVC toolset");
    testRandom(); testSpecial(); testGuardPage();
    puts("PASS: bit-exact bounds across random geometry, tails, repeated indices, signed zeros, non-finite values and guarded vertex storage");
    if (argc > 1) benchmark();
}
