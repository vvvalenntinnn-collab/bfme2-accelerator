// Positive lookup index for the retail manager's existing ordered handle list.
// Nodes remain owned by that list; returned handles use the original copy ctor.
#define RW_ID_N 8192u
#define RW_ID_PROBES 32u
struct RwIdEntry { DWORD id; BYTE* node; };
struct RwIdIndex {
    RwIdEntry ids[RW_ID_N];
    DWORD handles[RW_ID_N];
    BYTE* owner; BYTE* head;
    DWORD queries, recentQueries, buildAfter;
    bool ready;
};
typedef DWORD (__fastcall* RwFindFn)(void*,void*,void*,DWORD);
typedef DWORD (__fastcall* RwCopyFn)(void*,void*,const void*);
typedef DWORD (__fastcall* RwNodeFn)(void*,void*,const void*);
typedef DWORD (__fastcall* RwEraseFn)(void*,void*,void*,void*);
typedef void (__fastcall* RwClearFn)(void*,void*);
typedef DWORD (__fastcall* RwAssignFn)(void*,void*,const void*);
typedef void (__cdecl* RwXferFn)(void*,void*);
static RwIdIndex* g_rwIds = NULL;
static RwFindFn g_rwFindOriginal = NULL;
static RwCopyFn g_rwHandleCopy = (RwCopyFn)0x0044C4BE;
static RwNodeFn g_rwNodeOriginal = NULL;
static RwEraseFn g_rwEraseOriginal = NULL;
static RwClearFn g_rwClearOriginal = NULL;
static RwAssignFn g_rwAssignOriginal = NULL;
static RwXferFn g_rwXferOriginal = NULL;
static volatile LONG g_rwIdKill = 0;
static DWORD g_rwIdProof = 512, g_rwIdTick = 0;
static DWORD g_rwIdMutationDepth = 0;
static __forceinline DWORD rwIdHash(DWORD key) { return (key * 2654435761u) >> 19; }
static void rwIdInvalidate() {
    if (!g_rwIds) return;
    if (g_mkTid && GetCurrentThreadId() != g_mkTid) {
        if (!InterlockedExchange(&g_rwIdKill,1)) logf("rotwk: foreign-thread particle-list mutation - ID index OFF.");
        return;
    }
    // A burst that ends at eight lookups paid for a build and barely used it.
    // Delay the next build to 32 after short bursts; longer quiet windows
    // restore the eight-query startup policy. Saturation keeps steady hits lean.
    if(g_rwIds->recentQueries) g_rwIds->buildAfter=g_rwIds->recentQueries<=8?32:8;
    g_rwIds->ready = false; g_rwIds->queries = g_rwIds->recentQueries = 0;
}
static bool rwIdTrackedHandle(const void* handle) {
    if (!g_rwIds || !g_rwIds->ready) return false;
    DWORD key = (DWORD)(ULONG_PTR)handle, slot = rwIdHash(key);
    for (DWORD i = 0; i < RW_ID_PROBES; ++i) {
        DWORD at = g_rwIds->handles[(slot+i)&(RW_ID_N-1)];
        if (at == key) return true;
        if (!at) return false;
    }
    return false;
}
static bool rwIdBuild() {
    RwIdIndex& ix = *g_rwIds;
    memset(ix.ids,0,sizeof(ix.ids)); memset(ix.handles,0,sizeof(ix.handles));
    BYTE* node = *(BYTE**)ix.head;
    DWORD count = 0;
    for (; node != ix.head; node = *(BYTE**)node) {
        if (!node || ++count > RW_ID_N/2) return false;
        DWORD handle = (DWORD)(ULONG_PTR)(node+8), slot = rwIdHash(handle);
        bool placed = false;
        for (DWORD i = 0; i < RW_ID_PROBES; ++i) {
            DWORD& key = ix.handles[(slot+i)&(RW_ID_N-1)];
            if (!key) { key = handle; placed = true; break; }
        }
        if (!placed) return false;
        BYTE* system = *(BYTE**)(node+8);
        if (!system) continue;
        DWORD id = *(DWORD*)(system+0xA8);
        if (!id) continue;
        slot = rwIdHash(id); placed = false;
        for (DWORD i = 0; i < RW_ID_PROBES; ++i) {
            RwIdEntry& e = ix.ids[(slot+i)&(RW_ID_N-1)];
            if (!e.node) { e.id = id; e.node = node; placed = true; break; }
            if (e.id == id) { placed = true; break; } // retain the first node, as stock does
        }
        if (!placed) return false;
    }
    return count >= 32; // keep the stock walk for small lists
}
static BYTE* rwIdFind(DWORD id) {
    DWORD slot = rwIdHash(id);
    for (DWORD i = 0; i < RW_ID_PROBES; ++i) {
        const RwIdEntry& e = g_rwIds->ids[(slot+i)&(RW_ID_N-1)];
        if (!e.node) return NULL;
        if (e.id == id) return e.node;
    }
    return NULL;
}
static DWORD __fastcall hkRwFind(void* self, void* edx, void* output, DWORD id) {
    if (!id || g_rwIdKill || !g_mkTid || GetCurrentThreadId() != g_mkTid || g_rwIdMutationDepth)
        return g_rwFindOriginal(self,edx,output,id);
    RwIdIndex& ix = *g_rwIds;
    BYTE* head = *(BYTE**)((BYTE*)self+0x4C);
    if (ix.owner != self || ix.head != head) {
        rwIdInvalidate(); ix.owner = (BYTE*)self; ix.head = head;
        ix.buildAfter=8;
    }
    if(ix.recentQueries<9) ++ix.recentQueries;
    // Avoid rebuilding for rapidly changing lists: the first seven queries
    // after a mutation keep stock behavior (31 after recent short bursts).
    // A failed build retries only on mutation, not on every later query.
    DWORD threshold=ix.buildAfter?ix.buildAfter:8;
    if (!ix.ready && ix.queries < threshold && ++ix.queries == threshold && head) ix.ready = rwIdBuild();
    BYTE* node = ix.ready ? rwIdFind(id) : NULL;
    if (!node) return g_rwFindOriginal(self,edx,output,id); // never cache misses
    BYTE* system = *(BYTE**)(node+8);
    if (!system || *(DWORD*)(system+0xA8) != id) {
        rwIdInvalidate(); return g_rwFindOriginal(self,edx,output,id);
    }
    if (g_rwIdProof || ((++g_rwIdTick & 63) == 0)) {
        DWORD result = g_rwFindOriginal(self,edx,output,id);
        if (*(void**)output != system) {
            InterlockedExchange(&g_rwIdKill,1);
            logf("rotwk: particle ID lookup mismatch - index OFF for this session.");
        } else if (g_rwIdProof && !--g_rwIdProof)
            logf("rotwk: 512 particle ID hits matched; index live (1 in 64 checked).");
        return result;
    }
    return g_rwHandleCopy(output,NULL,node+8);
}
static DWORD __fastcall hkRwNode(void* self,void* edx,const void* value) {
    rwIdInvalidate();
    bool local = !g_mkTid || GetCurrentThreadId() == g_mkTid;
    if (local) ++g_rwIdMutationDepth;
    DWORD result = g_rwNodeOriginal(self,edx,value);
    if (local) --g_rwIdMutationDepth;
    rwIdInvalidate(); // an allocator callback must not seed a pre-insertion snapshot
    return result;
}
static DWORD __fastcall hkRwErase(void* self,void* edx,void* output,void* node) {
    bool tracked = g_rwIds && g_rwIds->owner && (BYTE*)self == g_rwIds->owner+0x4C;
    bool local = !g_mkTid || GetCurrentThreadId() == g_mkTid;
    if (tracked) { rwIdInvalidate(); if (local) ++g_rwIdMutationDepth; }
    DWORD result = g_rwEraseOriginal(self,edx,output,node);
    if (tracked) { if (local) --g_rwIdMutationDepth; rwIdInvalidate(); }
    return result;
}
static void __fastcall hkRwClear(void* self,void* edx) {
    bool tracked = g_rwIds && g_rwIds->owner && (BYTE*)self == g_rwIds->owner+0x4C;
    bool local = !g_mkTid || GetCurrentThreadId() == g_mkTid;
    if (tracked) { rwIdInvalidate(); if (local) ++g_rwIdMutationDepth; }
    g_rwClearOriginal(self,edx);
    if (tracked) { if (local) --g_rwIdMutationDepth; rwIdInvalidate(); }
}
static DWORD __fastcall hkRwAssign(void* self,void* edx,const void* value) {
    bool local = !g_mkTid || GetCurrentThreadId() == g_mkTid;
    if (!local) rwIdInvalidate(); // avoid reading a snapshot being rebuilt on the producer
    bool tracked = local && rwIdTrackedHandle(self);
    if (tracked) { rwIdInvalidate(); ++g_rwIdMutationDepth; }
    DWORD result = g_rwAssignOriginal(self,edx,value);
    if (tracked) { --g_rwIdMutationDepth; rwIdInvalidate(); }
    return result;
}
static void __cdecl hkRwXfer(void* xfer,void* value) {
    rwIdInvalidate();
    bool local = !g_mkTid || GetCurrentThreadId() == g_mkTid;
    if (local) ++g_rwIdMutationDepth;
    g_rwXferOriginal(xfer,value);
    if (local) --g_rwIdMutationDepth;
    rwIdInvalidate();
}
