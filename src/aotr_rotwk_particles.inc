// Verified RotWK .text hashes 4404F3C6 and 88C193EE; target bodies match both.
#include "aotr_particle_math.h"
#include "aotr_prepare_pool.h"
#define RW_COLOR_MAX 65536u
typedef void (__fastcall* RwColorFn)(void*, void*);
static RwColorFn g_rwColorOriginal = NULL;
static float* g_rwColorScratch = NULL;
static DWORD g_rwColorProof = 512, g_rwColorTick = 0;
static bool g_rwColorKill = false;
static AccelPreparePool g_rwPreparePool={};
static bool g_rwPrepareTried=false, g_rwPrepareEnabled=false;
static bool rwStartPreparationPool() {
    if(!g_rwPrepareTried) {
        g_rwPrepareTried=true;
        AccelCpuPlan plan=accelReserveCpuRoles(accelAllowedCpuPlan(),g_rtIdealMain,g_rtIdealWorker);
        g_rwPreparePool.start(plan);
        logf("rotwk: joined preparation pool %u workers on %u allowed physical cores (two cores reserved for main/render).",g_rwPreparePool.workers,plan.count);
    }
    return g_rwPreparePool.workers!=0;
}
struct RwColorJob { float* dst; const float* rgb; const float* alpha; };
static void rwColorRange(void* context,unsigned begin,unsigned end) {
    const RwColorJob& job=*(RwColorJob*)context;
    particleColorPack(job.dst+begin*4,job.rgb?job.rgb+begin*3:NULL,job.alpha?job.alpha+begin:NULL,end-begin);
}
static bool rwRunPreparation(AccelPrepareFn function,void* context,DWORD count) {
    // Dispatch/join cost dominates ordinary small systems. Experimental large
    // packs use the same shared pool intended for future pure preparation jobs.
    if(g_rwPrepareEnabled && count>=32768) {
        return rwStartPreparationPool() && g_rwPreparePool.run(function,context,count,2048);
    }
    return false;
}
static void rwPackColors(float* dst,const float* rgb,const float* alpha,DWORD count) {
    RwColorJob job={dst,rgb,alpha};
    if(rwRunPreparation(rwColorRange,&job,count)) return;
    particleColorPack(dst,rgb,alpha,count);
}
static bool rwOverlap(const void* a, DWORD an, const void* b, DWORD bn) {
    if (!b) return false;
    ULONG_PTR x = (ULONG_PTR)a, y = (ULONG_PTR)b;
    return x <= y ? y - x < an : x - y < bn;
}
static void __fastcall hkRwParticleColor(void* object, void* edx) {
    // Stock owns allocation, default-only release, exceptional layouts, and
    // foreign-thread calls. Keeping all capacity slots also covers line tails.
    if (g_rwColorKill || !g_mkTid || GetCurrentThreadId() != g_mkTid || (_mm_getcsr() & 0x1F80) != 0x1F80) {
        g_rwColorOriginal(object, edx); return;
    }
    BYTE* self = (BYTE*)object;
    DWORD count = *(DWORD*)(self + 0x100);
    BYTE* diffuse = *(BYTE**)(self + 0x218);
    BYTE* color = *(BYTE**)(self + 0x21C);
    BYTE* opacity = *(BYTE**)(self + 0x220);
    if (!diffuse || (!color && !opacity) || count > RW_COLOR_MAX) {
        g_rwColorOriginal(object, edx); return;
    }
    float* dst = *(float**)(diffuse + 12);
    const float* rgb = color ? *(float**)(color + 12) : NULL;
    const float* alpha = opacity ? *(float**)(opacity + 12) : NULL;
    if (!dst || (color && !rgb) || (opacity && !alpha) ||
        rwOverlap(dst, count * 16, rgb, count * 12) || rwOverlap(dst, count * 16, alpha, count * 4)) {
        g_rwColorOriginal(object, edx); return;
    }
    bool check = g_rwColorProof || ((++g_rwColorTick & 63) == 0);
    if (check) {
        rwPackColors(g_rwColorScratch, rgb, alpha, count);
        g_rwColorOriginal(object, edx);
        // Return the original output on every proof/verification call.
        if (memcmp(dst, g_rwColorScratch, count * 16)) {
            g_rwColorKill = true;
            logf("rotwk: particle color mismatch - fused pass OFF for this session.");
        } else if (g_rwColorProof && !--g_rwColorProof) {
            logf("rotwk: 512 complete particle color outputs matched; fused pass live (1 in 64 checked).");
        }
    } else rwPackColors(dst, rgb, alpha, count);
}
static DWORD rwBodyHash(const BYTE* bytes, DWORD size) {
    DWORD hash = 2166136261u;
    for (DWORD i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 16777619u; }
    return hash;
}
#include "aotr_rotwk_pose.inc"
#include "aotr_rotwk_particleindex.inc"

struct RwHook {
    DWORD address, size, digest;
    int stolen;
    void* replacement;
    BYTE* trampoline;
    BYTE saved[8];
};
static bool rwBodiesMatch(const RwHook* hooks, int count) {
    __try {
        for (int i = 0; i < count; ++i)
            if (rwBodyHash((const BYTE*)hooks[i].address,hooks[i].size) != hooks[i].digest) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool rwPrepareHooks(RwHook* hooks, int count) {
    if (!rwBodiesMatch(hooks,count)) return false;
    for (int i = 0; i < count; ++i) {
        memcpy(hooks[i].saved,(const void*)hooks[i].address,hooks[i].stolen);
        hooks[i].trampoline = makeTrampoline((BYTE*)hooks[i].address,hooks[i].stolen);
        if (!hooks[i].trampoline) return false;
        FlushInstructionCache(GetCurrentProcess(),hooks[i].trampoline,64);
    }
    return true;
}
// Abort rather than patch a suspended instruction pointer inside a stolen
// prologue. Every lifetime hook is prepared before any ID lookup is enabled.
static bool rwFreeze(HANDLE* threads, int* count, const RwHook* hooks, int hookCount) {
    *count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te = {}; te.dwSize = sizeof(te);
    bool ok = Thread32First(snap,&te) != FALSE;
    if (ok) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        if (*count == 256) { ok = false; break; }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,FALSE,te.th32ThreadID);
        if (!thread) { ok = false; break; }
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); ok = false; break; }
        threads[(*count)++] = thread;
        CONTEXT context = {}; context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread,&context)) { ok = false; break; }
        for (int i = 0; i < hookCount; ++i)
            if (context.Eip >= hooks[i].address && context.Eip - hooks[i].address < (DWORD)hooks[i].stolen) ok = false;
        if (!ok) break;
    } while (Thread32Next(snap,&te));
    CloseHandle(snap);
    if (!ok) { resumeAll(threads,*count); *count = 0; }
    return ok;
}
static bool rwRestore(const RwHook& hook) {
    DWORD old = 0, ignored;
    BYTE* target = (BYTE*)hook.address;
    if (!VirtualProtect(target,hook.stolen,PAGE_EXECUTE_READWRITE,&old)) return false;
    memcpy(target,hook.saved,hook.stolen);
    VirtualProtect(target,hook.stolen,old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),target,hook.stolen);
    return true;
}
typedef BOOL (*RwPatchFn)(BYTE*,void*,int);
static bool rwCommitHooks(RwHook* hooks, int count, RwPatchFn patch = patchJmp) {
    HANDLE frozen[256]; int threadCount;
    if (!rwFreeze(frozen,&threadCount,hooks,count)) return false;
    bool ok = rwBodiesMatch(hooks,count);
    int installed = 0;
    if (ok) for (; installed < count; ++installed) {
        if (!patch((BYTE*)hooks[installed].address,hooks[installed].replacement,hooks[installed].stolen)) {
            ok = false; break;
        }
    }
    bool restored = true;
    if (!ok) for (int i = installed-1; i >= 0; --i) if (!rwRestore(hooks[i])) restored = false;
    // Even a failed rollback leaves the ID fast path disabled. Any remaining
    // wrappers forward through their prepared originals and cannot use nodes.
    if (!ok && count > 1) InterlockedExchange(&g_rwIdKill,1);
    resumeAll(frozen,threadCount);
    if (!restored) logf("rotwk: detour rollback incomplete; prepared original forwarding retained.");
    return ok;
}
static bool rwEnabled(const char* name) {
    char value[8] = {};
    return !GetEnvironmentVariableA(name,value,sizeof(value)) || value[0] != '0';
}
static bool rwOptIn(const char* environment,const char* marker) {
    char value[8]={},path[MAX_PATH];
    if(GetEnvironmentVariableA(environment,value,sizeof(value))) return value[0]=='1';
    return GetFileAttributesA(aotrPath(path,marker))!=INVALID_FILE_ATTRIBUTES;
}
#include "aotr_rotwk_gather.inc"
static void installRotwkFxHooks() {
    // Parent gate requires complete .text hash 4404F3C6 or 88C193EE at image base
    // 400000. Check complete target bodies again immediately before patching.
    g_rwPrepareEnabled=rwOptIn("AOTR_PARTICLEWORKERS","PARTICLE_WORKERS_ON");
    installRotwkGather();
    if (rwEnabled("AOTR_PARTICLECOLOR")) {
        RwHook color = {0x5A7DF0,357,0x12FD16CD,6,(void*)hkRwParticleColor};
        g_rwColorScratch = (float*)VirtualAlloc(NULL,RW_COLOR_MAX*16,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        bool ok = g_rwColorScratch && rwPrepareHooks(&color,1);
        if (ok) { g_rwColorOriginal = (RwColorFn)color.trampoline; ok = rwCommitHooks(&color,1); }
        logf("rotwk: particle color fused copy/clamp %s (512 proofs, 1 in 64 checked).",ok ? "hooked" : "OFF");
    }
    char poseOptIn[8] = {};
    // The prototype matches retail output but does not consistently beat it.
    // Keep it available for explicit experiments, never enable it by default.
    if (GetEnvironmentVariableA("AOTR_POSEMATRIX",poseOptIn,sizeof(poseOptIn)) && poseOptIn[0]=='1') {
        RwHook pose = {0x59CD70,1283,0xD537820E,5,(void*)hkRwPose};
        bool ok = rwPrepareHooks(&pose,1);
        if (ok) { g_rwPoseOriginal = (RwPoseFn)pose.trampoline; ok = rwCommitHooks(&pose,1); }
        logf("rotwk: per-call HLod pivot matrix reuse %s; stock animation and child calls retained.",ok ? "hooked" : "OFF");
    }
    if (rwEnabled("AOTR_PARTICLEINDEX")) {
        RwHook ids[] = {
            {0x5F5487,34,0x6B01CE05,5,(void*)hkRwNode},
            {0x5F5BC9,42,0x1C2E8E2B,6,(void*)hkRwErase},
            {0x5F5DAF,51,0x78444B01,6,(void*)hkRwClear},
            {0x44C4E2,44,0xB7E77D2B,6,(void*)hkRwAssign},
            {0x7077DF,24,0xD86B267D,6,(void*)hkRwXfer},
            {0x5F530A,111,0x219D8714,5,(void*)hkRwFind}
        };
        g_rwIds = (RwIdIndex*)VirtualAlloc(NULL,sizeof(RwIdIndex),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        bool ok = g_rwIds && rwPrepareHooks(ids,6);
        if (ok) {
            g_rwNodeOriginal = (RwNodeFn)ids[0].trampoline;
            g_rwEraseOriginal = (RwEraseFn)ids[1].trampoline;
            g_rwClearOriginal = (RwClearFn)ids[2].trampoline;
            g_rwAssignOriginal = (RwAssignFn)ids[3].trampoline;
            g_rwXferOriginal = (RwXferFn)ids[4].trampoline;
            g_rwFindOriginal = (RwFindFn)ids[5].trampoline;
            ok = rwCommitHooks(ids,6);
        }
        if (!ok) InterlockedExchange(&g_rwIdKill,1);
        logf("rotwk: particle ID index %s; insertion/erase/clear/assignment/save-load hooks installed as a group.",ok ? "hooked (512 proofs)" : "OFF");
    }
}
