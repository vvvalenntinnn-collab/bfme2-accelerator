// Retail raw-animation preparation: no world writes or engine callbacks on
// workers. Keep native x87 root conversion/frame rounding on the main thread.
enum { RW_SKEL_JOBS=2048, RW_SKEL_BONES=131072, RW_SKEL_SEEN=8192 };
struct RwSkelFp { WORD control,p0,status,p1,tag,p2;DWORD words[4]; };
static RwSkelFp rwSkelSaveFp() {
    RwSkelFp e;
    __asm fnstenv e
    __asm fldenv e
    return e;
}
static void rwSkelRestoreFp(const RwSkelFp& e) {
    const RwSkelFp* p=&e;
    __asm {
        mov eax,p
        fldenv [eax]
    }
}
static LONG __declspec(naked) __cdecl rwSkelFrame(DWORD frame) {
    __asm {
        fld dword ptr [esp+4]
        fistp dword ptr [esp+4]
        mov eax,[esp+4]
        ret
    }
}
typedef float* (__cdecl* RwSkelQuatFn)(float*,const float*);
static RwSkelQuatFn g_rwSkelQuat=(RwSkelQuatFn)0xB2BD10;
typedef void (__fastcall* RwSkelKernelFn)(void*,void*,const float*,void*,LONG);
static RwSkelKernelFn g_rwSkelKernel=NULL;
static tThis0 g_rwSkelOriginal=NULL;
typedef void (__fastcall* RwSkelSceneFn)(void*,void*,void*);
static RwSkelSceneFn g_rwSkelSceneOriginal=NULL;
static volatile LONG g_rwSkelKill=0;
static bool g_rwSkelInstalled=false;
static unsigned g_rwSkelDepth=0,g_rwSkelCount=0,g_rwSkelProof=512,g_rwSkelTick=0;
static unsigned g_rwSkelTaken=0,g_rwSkelStale=0,g_rwSkelChecked=0;
static DWORD g_rwSkelGeneration=1;
struct RwSkelRoot { float quaternion[4],matrix[12]; };
static bool rwSkelBytes28(const BYTE* a,const BYTE* b) {
    __m128i x=_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)a),_mm_loadu_si128((const __m128i*)b));
    __m128i y=_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(a+12)),_mm_loadu_si128((const __m128i*)(b+12)));
    return _mm_movemask_epi8(_mm_and_si128(x,y))==65535;
}
static float* __cdecl rwSkelCopyRoot(float* out,const float* root) {
    _mm_storeu_ps(out,_mm_loadu_ps(root-4));return out;
}
static int __fastcall rwSkelFrames(void* clip,void*) { return *(int*)((BYTE*)clip+0x44); }
static int __fastcall rwSkelPivots(void* clip,void*) { return *(int*)((BYTE*)clip+0x48); }
static void* g_rwSkelClipVt[14]={};
struct RwSkelJob {
    BYTE* object;BYTE* pivots;DWORD count;BYTE inputs[88],treeInput[40],clipInput[84];
    RwSkelRoot root;LONG frame;DWORD csr,status;WORD control;BYTE ok,check;
    BYTE tree[40],clip[84];BYTE* after;
};
struct RwSkelSeen {
    void* object;DWORD frame,generation;unsigned job;
    BYTE* cache;BYTE* tree;BYTE* pivots;unsigned count;bool dirty;
};
static RwSkelJob* g_rwSkelJobs=NULL;
static RwSkelSeen* g_rwSkelSeen=NULL;
static BYTE* g_rwSkelAfter=NULL;
static unsigned g_rwSkelCapacity=0;
static unsigned g_rwSkelArenaUsed=0;
static bool g_rwSkelArenaReset=false;
static unsigned rwSkelSlot(void* object) { return ((DWORD)object>>4)*2654435761u>>(32-13); }
static void rwSkelRemember(void* object) {
    unsigned h=rwSkelSlot(object);
    for(unsigned i=0;i<8;++i) {
        RwSkelSeen& s=g_rwSkelSeen[(h+i)&(RW_SKEL_SEEN-1)];
        if(!s.object || s.object==object || (DWORD)g_mkFrames-s.frame>2) {
            if(s.object!=object) {s.generation=0;s.cache=NULL;}
            s.object=object;s.frame=g_mkFrames;return;
        }
    }
}
static RwSkelSeen* rwSkelFind(void* object) {
    unsigned h=rwSkelSlot(object);
    for(unsigned i=0;i<8;++i) {
        RwSkelSeen& s=g_rwSkelSeen[(h+i)&(RW_SKEL_SEEN-1)];
        if(s.object==object) return &s;
    }
    return NULL;
}
static bool rwSkelGrow(unsigned bones) {
    if(bones<=g_rwSkelCapacity) return true;
    unsigned n=(bones+4095)&~4095u;if(n>RW_SKEL_BONES) return false;
    if(!VirtualAlloc(g_rwSkelAfter,n*88,MEM_COMMIT,PAGE_READWRITE)) return false;
    g_rwSkelCapacity=n;return true;
}
static bool rwSkelLayout(BYTE* object,RwSkelJob& j,unsigned used,RwSkelSeen* cacheEntry=NULL) {
    if(!object || object[0xF4] || *(DWORD*)(object+0xFC) || *(DWORD*)(object+0x100)!=2 ||
       *(DWORD*)(object+0x110) || *(DWORD*)(object+0x7C) ||
       (*(DWORD**)object)[0xA8/4]!=0x59CD70) return false;
    BYTE* tree=*(BYTE**)(object+0xF8);BYTE* clip=*(BYTE**)(object+0x104);
    if(!tree || !clip || *(DWORD*)clip!=0xBEBD90 || *(DWORD*)(tree+0x1C)!=*(DWORD*)(tree+0x20)) return false;
    unsigned n=*(unsigned*)(tree+0x10);BYTE* pivots=*(BYTE**)(tree+0x14);
    if(n<2 || n>512 || !pivots || n>RW_SKEL_BONES-used || *(int*)(clip+0x44)<=0 ||
       *(int*)(clip+0x48)<(int)n || !*(void**)(clip+0x50)) return false;
    RwSkelSeen temporary={};if(!cacheEntry) cacheEntry=&temporary;
    bool fresh=!cacheEntry->cache || cacheEntry->tree!=tree || cacheEntry->pivots!=pivots || cacheEntry->count!=n;
    if(fresh) {
        if(n>RW_SKEL_BONES-g_rwSkelArenaUsed) {g_rwSkelArenaReset=true;return false;}
        if(!rwSkelGrow(g_rwSkelArenaUsed+n)) return false;
        cacheEntry->cache=g_rwSkelAfter+g_rwSkelArenaUsed*88;g_rwSkelArenaUsed+=n;
        cacheEntry->tree=tree;cacheEntry->pivots=pivots;cacheEntry->count=n;cacheEntry->dirty=true;
    }
    j.object=object;j.pivots=pivots;j.count=n;j.after=cacheEntry->cache;
    memcpy(j.inputs,object+0x18,48);memcpy(j.inputs+48,object+0xF8,40);
    memcpy(j.treeInput,tree,40);memcpy(j.clipInput,clip,84);
    memcpy(j.tree,j.treeInput,40);*(BYTE**)(j.tree+0x14)=j.after;
    *(DWORD*)(j.tree+0x1C)=*(DWORD*)(j.tree+0x20)=0;
    // Keep immutable hierarchy inputs in the private tree between passes.
    // They are still compared in full at consumption; a changed/reused engine
    // tree falls back and refreshes this cache before its next preparation.
    if(cacheEntry->dirty) for(unsigned i=0;i<n;++i) {
        const BYTE* p=pivots+i*88;BYTE* output=j.after+i*88;DWORD parent=*(DWORD*)(p+0x10),index=*(DWORD*)(p+0x4C);
        if(i && (parent<(DWORD)pivots || (parent-(DWORD)pivots)%88 || (parent-(DWORD)pivots)/88>=i || index==0xFFFFFFFF)) return false;
        __m128i x=_mm_loadu_si128((const __m128i*)(p+0x10)),y=_mm_loadu_si128((const __m128i*)(p+0x20));
        _mm_storeu_si128((__m128i*)(output+0x10),x);_mm_storeu_si128((__m128i*)(output+0x20),y);
        *(DWORD*)(output+0x4C)=index;
        if(i) *(BYTE**)(output+0x10)=j.after+(parent-(DWORD)pivots);
    }
    cacheEntry->dirty=false;
    memcpy(j.clip,j.clipInput,84);*(void***)j.clip=g_rwSkelClipVt;
    memcpy(j.root.matrix,j.inputs,48);j.ok=0;
    return true;
}
static void rwSkelRange(void* context,unsigned begin,unsigned end) {
    RwSkelJob* jobs=(RwSkelJob*)context;
    for(unsigned i=begin;i<end;++i) {
        RwSkelJob& j=jobs[i];DWORD saved=_mm_getcsr();_mm_setcsr(j.csr&~63u);
        __try {
            g_rwSkelKernel(j.tree,NULL,j.root.matrix,j.clip,j.frame);
            j.status|=_mm_getcsr()&63;j.ok=1;
        } __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_rwSkelKill,1); }
        _mm_setcsr(saved);
    }
}
static bool rwSkelUnchanged(const RwSkelJob& j,BYTE* object) {
    if(object[0xF4] || *(DWORD*)(object+0x7C) || (*(DWORD**)object)[0xA8/4]!=0x59CD70 ||
       memcmp(j.inputs,object+0x18,48) || memcmp(j.inputs+48,object+0xF8,40)) return false;
    BYTE* tree=*(BYTE**)(object+0xF8);BYTE* clip=*(BYTE**)(object+0x104);
    if(memcmp(j.treeInput,tree,40) || memcmp(j.clipInput,clip,84)) return false;
    for(unsigned i=0;i<j.count;++i) {
        const BYTE* p=j.pivots+i*88;const BYTE* input=j.after+i*88;
        if(!rwSkelBytes28(input+0x14,p+0x14) || *(DWORD*)(input+0x4C)!=*(DWORD*)(p+0x4C) ||
           (i && *(DWORD*)(input+0x10)!=(DWORD)j.after+(*(DWORD*)(p+0x10)-(DWORD)j.pivots))) return false;
    }
    return true;
}
static bool rwSkelEqual(const RwSkelJob& j) {
    for(unsigned i=0;i<j.count;++i) {
        const BYTE *a=j.pivots+i*88,*b=j.after+i*88;
        __m128i x=_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(a+0x30)),_mm_loadu_si128((const __m128i*)(b+0x30)));
        __m128i y=_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(a+0x3C)),_mm_loadu_si128((const __m128i*)(b+0x3C)));
        if(_mm_movemask_epi8(_mm_and_si128(x,y))!=65535 || a[0x50]!=b[0x50] || *(DWORD*)(a+0x54)!=*(DWORD*)(b+0x54)) return false;
    }
    return true;
}
static void __fastcall hkRwSkeleton(void* object,void* edx) {
    if(!g_rwSkelInstalled || !g_mkTid || GetCurrentThreadId()!=g_mkTid || g_rwSkelKill) {
        g_rwSkelOriginal(object,edx);return;
    }
    rwSkelRemember(object);
    RwSkelSeen* entry=rwSkelFind(object);
    if(entry && entry->generation==g_rwSkelGeneration && entry->job<g_rwSkelCount) {
        RwSkelJob& j=g_rwSkelJobs[entry->job];
        if(!j.ok) {g_rwSkelOriginal(object,edx);return;}
        j.ok=0;RwSkelFp fp=rwSkelSaveFp();DWORD csr=_mm_getcsr();bool same=false;
        __try {same=fp.tag==0xFFFF && fp.control==j.control && (csr&~63u)==(j.csr&~63u) && rwSkelUnchanged(j,(BYTE*)object);}
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        if(!same) {entry->dirty=true;++g_rwSkelStale;g_rwSkelOriginal(object,edx);return;}
        if(j.check) {
            g_rwSkelOriginal(object,edx);
            if(!rwSkelEqual(j) || ((csr|j.status)&63)!=(_mm_getcsr()&63) || !((BYTE*)object)[0xF4]) {
                InterlockedExchange(&g_rwSkelKill,1);logf("rotwk: skeleton worker proof mismatch; native updates retained for this session.");
            } else {
                ++g_rwSkelChecked;
                if(g_rwSkelProof && !--g_rwSkelProof) logf("rotwk: 512 private skeleton results matched native; worker preparation live (1 in 64 checked).");
            }
        } else {
            // Reproduce the two x87 operations at their original consumption
            // point, including their precision, rounding and status effects.
            float root[4];g_rwSkelQuat(root,(const float*)((BYTE*)object+0x18));
            rwSkelFrame(*(DWORD*)((BYTE*)object+0x108));
            for(unsigned bone=0;bone<j.count;++bone) {
                BYTE* out=j.pivots+bone*88;const BYTE* in=j.after+bone*88;
                _mm_storeu_si128((__m128i*)(out+0x30),_mm_loadu_si128((const __m128i*)(in+0x30)));
                _mm_storeu_si128((__m128i*)(out+0x3C),_mm_loadu_si128((const __m128i*)(in+0x3C)));
                out[0x50]=in[0x50];*(DWORD*)(out+0x54)=*(DWORD*)(in+0x54);
            }
            ((BYTE*)object)[0xF4]=1;_mm_setcsr(_mm_getcsr()|j.status);++g_rwSkelTaken;
        }
        return;
    }
    g_rwSkelOriginal(object,edx);
}
static void rwSkelCollect(void* scene) {
    g_rwSkelCount=0;RwSkelFp fp=rwSkelSaveFp();DWORD csr=_mm_getcsr();
    if(fp.tag!=0xFFFF || (fp.control&63)!=63 || (csr&0x1F80)!=0x1F80 || !rwStartPreparationPool()) return;
    unsigned used=0;
    if(g_rwSkelArenaReset) {
        for(unsigned i=0;i<RW_SKEL_SEEN;++i) g_rwSkelSeen[i].cache=NULL;
        g_rwSkelArenaUsed=0;g_rwSkelArenaReset=false;
    }
    if(!++g_rwSkelGeneration) {
        memset(g_rwSkelSeen,0,sizeof(RwSkelSeen)*RW_SKEL_SEEN);g_rwSkelArenaUsed=0;g_rwSkelArenaReset=false;++g_rwSkelGeneration;
    }
    __try {
        BYTE* head=(BYTE*)scene+0xF0;BYTE* node=*(BYTE**)(head+4);
        for(unsigned visited=0;node && node!=head && visited<200000 && g_rwSkelCount<RW_SKEL_JOBS;++visited) {
            BYTE* object=*(BYTE**)(node+0xC);node=*(BYTE**)(node+4);if(!object) continue;object-=8;
            RwSkelSeen* entry=rwSkelFind(object);
            if(!entry || (DWORD)g_mkFrames-entry->frame>2 || entry->generation==g_rwSkelGeneration) continue;
            RwSkelJob& j=g_rwSkelJobs[g_rwSkelCount];if(!rwSkelLayout(object,j,used,entry)) continue;
            used+=j.count;j.control=fp.control;j.csr=csr;_mm_setcsr(csr&~63u);
            g_rwSkelQuat(j.root.quaternion,j.root.matrix);j.frame=rwSkelFrame(*(DWORD*)(object+0x108));j.status=_mm_getcsr()&63;
            j.check=(BYTE)(g_rwSkelProof || ((++g_rwSkelTick&63)==0));
            entry->generation=g_rwSkelGeneration;entry->job=g_rwSkelCount++;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {g_rwSkelCount=0;}
    if(g_rwSkelCount<32 || used<2048 || !g_rwPreparePool.run(rwSkelRange,g_rwSkelJobs,g_rwSkelCount,8)) g_rwSkelCount=0;
    if(g_rwSkelKill) {g_rwSkelCount=0;logf("rotwk: private skeleton worker fault; native updates retained for this session.");}
    // Speculative/cull-rejected/stale jobs must not leak FP flags into gameplay.
    rwSkelRestoreFp(fp);_mm_setcsr(csr);
}
static void __fastcall hkRwSkeletonScene(void* scene,void* edx,void* rinfo) {
    if(!g_mkTid || GetCurrentThreadId()!=g_mkTid) {g_rwSkelSceneOriginal(scene,edx,rinfo);return;}
    bool outer=g_rwSkelInstalled && !g_rwSkelDepth && !g_rwSkelKill && g_mkTid && GetCurrentThreadId()==g_mkTid;
    if(outer) rwSkelCollect(scene);
    ++g_rwSkelDepth;
    __try {g_rwSkelSceneOriginal(scene,edx,rinfo);}
    __finally {--g_rwSkelDepth;if(outer) g_rwSkelCount=0;}
}
static const RwRegion kRwSkelRegions[]={
    {0x563310,2850,0xFFFABD60},{0x5A5050,345,0x4B906A19},{0x47177E,733,0x5E1E397E},
    {0xB2BD10,360,0xC1F7C2D6},{0x5A3F70,109,0xAD1CEA1A},{0xBEBD90,88,0x82D45E23},
    {0x58E9C0,4,0x60A96AA4},{0x58E9F0,4,0x78B28AC8},{0x5175A0,3,0x94777CFF},{0xA88AC0,1,0x460B3072}
};
static bool rwSkelMakeKernel() {
    BYTE* copy=(BYTE*)VirtualAlloc(NULL,2850,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!copy) return false;
    memcpy(copy,(void*)0x563310,2850);
    // Same-sized instruction replacement. Caller supplies native-rounded frame
    // bits and native-converted root quaternion, so the worker path is SSE-only.
    const BYTE frame[]={0x8B,0x44,0x24,0x6C,0x89,0x44,0x24,0x64};memcpy(copy+0x64,frame,8);
    *(LONG*)(copy+0x1E)=(LONG)((BYTE*)rwSkelCopyRoot-(copy+0x22));
    *(LONG*)(copy+0xAF3)=(LONG)((BYTE*)0x561B40-(copy+0xAF7));
    DWORD old;if(!VirtualProtect(copy,2850,PAGE_EXECUTE_READ,&old)) {VirtualFree(copy,0,MEM_RELEASE);return false;}
    FlushInstructionCache(GetCurrentProcess(),copy,2850);g_rwSkelKernel=(RwSkelKernelFn)copy;
    g_rwSkelClipVt[0x14/4]=(void*)rwSkelFrames;g_rwSkelClipVt[0x34/4]=(void*)rwSkelPivots;return true;
}
static bool rwInstallSkeletonWorkers() {
    if(!rwEnabled("AOTR_SKELETONWORKERS") || accelAllowedCpuPlan().count<3 ||
       !rwRegionsMatch(kRwSkelRegions,_countof(kRwSkelRegions)) || !rwSkelMakeKernel()) return false;
    g_rwSkelJobs=(RwSkelJob*)VirtualAlloc(NULL,sizeof(RwSkelJob)*RW_SKEL_JOBS,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_rwSkelSeen=(RwSkelSeen*)VirtualAlloc(NULL,sizeof(RwSkelSeen)*RW_SKEL_SEEN,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g_rwSkelAfter=(BYTE*)VirtualAlloc(NULL,RW_SKEL_BONES*88,MEM_RESERVE,PAGE_READWRITE);
    if(!g_rwSkelJobs || !g_rwSkelSeen || !g_rwSkelAfter) return false;
    RwHook hooks[]={{0x47177E,733,0x5E1E397E,6,(void*)hkRwSkeletonScene},{0x5A5050,345,0x4B906A19,6,(void*)hkRwSkeleton}};
    if(!rwPrepareHooks(hooks,2)) return false;
    g_rwSkelSceneOriginal=(RwSkelSceneFn)hooks[0].trampoline;g_rwSkelOriginal=(tThis0)hooks[1].trampoline;
    RwEdit edits[]={rwBranchEdit(0x47177E,6,0xE9,(void*)hkRwSkeletonScene),rwBranchEdit(0x5A5050,6,0xE9,(void*)hkRwSkeleton)};
    g_rwSkelInstalled=rwCommitEdits(edits,2,kRwSkelRegions,_countof(kRwSkelRegions));return g_rwSkelInstalled;
}
