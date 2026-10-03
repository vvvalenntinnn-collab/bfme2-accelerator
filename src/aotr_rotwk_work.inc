// Independent capabilities for the supplied retail/FPS-patched ROTWK pair.
// Regions contain hashes/addresses only; no executable bytes are distributed.
struct RwRegion { DWORD address, size, digest; };
static bool rwRegionsMatch(const RwRegion* regions, int count) {
    __try {
        for (int i=0;i<count;++i)
            if (rwBodyHash((const BYTE*)regions[i].address,regions[i].size)!=regions[i].digest) return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct RwEdit { DWORD address; int size; BYTE before[64], after[64]; };
static bool rwWriteEdit(DWORD address,const BYTE* bytes,int size) {
    DWORD old,ignored;
    if (!VirtualProtect((void*)address,size,PAGE_EXECUTE_READWRITE,&old)) return false;
    memcpy((void*)address,bytes,size);
    BOOL restored=VirtualProtect((void*)address,size,old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),(void*)address,size);
    return restored!=FALSE;
}
typedef bool (*RwEditWriter)(DWORD,const BYTE*,int);
static bool rwCommitEdits(RwEdit* edits,int count,const RwRegion* regions,int nregions,RwEditWriter write=rwWriteEdit) {
    if (count<1 || count>48 || !rwRegionsMatch(regions,nregions)) return false;
    RwHook guards[48]={};
    for (int i=0;i<count;++i) {
        if (edits[i].size<1 || edits[i].size>64) return false;
        guards[i].address=edits[i].address; guards[i].stolen=edits[i].size;
        memcpy(edits[i].before,(const void*)edits[i].address,edits[i].size);
    }
    HANDLE frozen[256]; int nf;
    if (!rwFreeze(frozen,&nf,guards,count)) return false;
    bool ok=rwRegionsMatch(regions,nregions);
    for (int i=0;i<count && ok;++i)
        ok=!memcmp((const void*)edits[i].address,edits[i].before,edits[i].size);
    int attempted=0;
    if (ok) for (;attempted<count;++attempted) {
        if (!write(edits[attempted].address,edits[attempted].after,edits[attempted].size)) { ++attempted; ok=false; break; }
    }
    if (!ok) for (int i=attempted-1;i>=0;--i)
        if (!rwWriteEdit(edits[i].address,edits[i].before,edits[i].size))
            logf("rotwk-work: rollback failed at %08X; fast paths remain disabled.",edits[i].address);
    resumeAll(frozen,nf);
    return ok;
}
static RwEdit rwBranchEdit(DWORD address,int size,BYTE op,void* target) {
    RwEdit e={}; e.address=address; e.size=size; memset(e.after,0x90,size);
    e.after[0]=op; *(LONG*)(e.after+1)=(LONG)((DWORD)(ULONG_PTR)target-address-5);
    return e;
}
static const RwRegion kRwListRegions[]={
    {0x00572D07,6000,0x19F953F8}, // sort, comparators, heap fallback, push and erase dependencies
    {0x0047AF67,16,0x2DA9B0EB}   // RefPtr release body
};
static const RwRegion kRwAudioRegions[]={
    {0x00453D00,57600,0x56EEE867}, // manager, full limit/pass routines and all 35 mutation sites
    {0x0060011F,100,0x266314F9}, {0x0086DC96,100,0x3B148F4E},
    {0x00702055,100,0x7ABC9597}, {0x0094EB6C,100,0x57A276A9}, {0x005FFE47,100,0xCE5D1552}
};
static bool rwInstallList() {
    if (!rwEnabled("AOTR_RLSORT") || !rwRegionsMatch(kRwListRegions,2)) return false;
    RwHook hooks[]={
        {0x5740D0,67,0x43E271F1,5,(void*)hkRlSort},
        {0x57430D,249,0xA6ED392F,5,(void*)hkRlPush},
        {0x573816,51,0xCD56C32A,5,(void*)hkRlErase}
    };
    if (!rwPrepareHooks(hooks,3)) return false;
    o_rlSort=(tRlSort)hooks[0].trampoline; o_rlPush=(tRlPush)hooks[1].trampoline; o_rlErase=(tRlErase)hooks[2].trampoline;
    double a=0,b=0; int lists=0,heap=0;
    bool good=false;
    __try { good=rlSelfTest(&a,&b,&lists,&heap); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if (!good) return false;
    RwEdit edits[3];
    for (int i=0;i<3;++i) edits[i]=rwBranchEdit(hooks[i].address,hooks[i].stolen,0xE9,hooks[i].replacement);
    if (!rwCommitEdits(edits,3,kRwListRegions,2)) return false;
    g_rlLive=1; g_rlPushLive=rwEnabled("AOTR_RLPUSH")?1:0;
    logf("rotwk-work: render-list sort/push/erase LIVE (%d startup lists identical; stock draw/batch order retained).",lists);
    return true;
}
static bool rwPrepareAudioEdits(RwEdit* edits,RwHook* pass) {
    if (!rwRegionsMatch(kRwAudioRegions,_countof(kRwAudioRegions)) || !rwPrepareHooks(pass,1)) return false;
    o_alPass=(tAlPass)pass->trampoline;
    for (int i=0;i<AL_NSITE;++i) {
        const BYTE* p=(const BYTE*)kAlSite[i].at;
        if (p[0]!=0xE8 || kAlSite[i].at+5+*(const LONG*)(p+1)!=kAlSite[i].target) return false;
        edits[i]=rwBranchEdit(kAlSite[i].at,5,0xE8,kAlSite[i].thunk);
    }
    edits[AL_NSITE]=rwBranchEdit(pass->address,5,0xE9,pass->replacement);
    RwEdit& loop=edits[AL_NSITE+1]; loop={}; loop.address=0x456E88; loop.size=54;
    memset(loop.after,0x90,54);
    const BYTE stub[]={0x57,0x53,0xE8,0,0,0,0,0x01,0x45,0xFC,0x01,0x45,0x08,0xEB,0x27};
    memcpy(loop.after,stub,sizeof(stub)); *(LONG*)(loop.after+3)=(LONG)((DWORD)(ULONG_PTR)alCount-0x456E8F);
    return true;
}
static bool rwInstallAudio() {
    if (!rwEnabled("AOTR_AUDIOLIMIT")) return false;
    RwHook pass={0x461CA3,259,0x2475C0B5,5,(void*)hkAlPass};
    RwEdit edits[AL_NSITE+2];
    if(!rwPrepareAudioEdits(edits,&pass)) return false;
    if (!rwCommitEdits(edits,AL_NSITE+2,kRwAudioRegions,_countof(kRwAudioRegions))) return false;
    g_alLive=1;
    logf("rotwk-work: audio request index LIVE (full manager region verified, %d mutation sites committed together).",AL_NSITE);
    return true;
}

#ifndef AOTR_PROD
enum { RW_LOGIC, RW_CLIENT, RW_PARTICLES, RW_PATH, RW_PHASES };
static const char* kRwPhaseName[RW_PHASES]={"logic","client","particle-manager","path-queue"};
static volatile LONG64 g_rwPhaseTicks[RW_PHASES];
static volatile LONG g_rwPhaseCalls[RW_PHASES];
static tEngLogic g_rwLogicOriginal;
static tEngClient g_rwClientOriginal;
static tThis0 g_rwManagerOriginal,g_rwPathOriginal;
static void rwPhaseRecord(int phase,LONG64 start) {
    InterlockedExchangeAdd64(&g_rwPhaseTicks[phase],qpcNow()-start);
    InterlockedIncrement(&g_rwPhaseCalls[phase]);
}
static void __fastcall hkRwLogic(void* self,void* edx,DWORD n) {
    bool main=g_mkTid && GetCurrentThreadId()==g_mkTid;LONG prev=g_phase;
    if(main) g_phase=PH_LOGIC;LONG64 t=qpcNow();g_rwLogicOriginal(self,edx,n);rwPhaseRecord(RW_LOGIC,t);if(main) g_phase=prev;
}
static void __fastcall hkRwClient(void* self,void* edx) {
    bool main=g_mkTid && GetCurrentThreadId()==g_mkTid;LONG prev=g_phase;
    if(main) g_phase=PH_CLIENT;LONG64 t=qpcNow();g_rwClientOriginal(self,edx);rwPhaseRecord(RW_CLIENT,t);if(main) g_phase=prev;
}
static void __fastcall hkRwManager(void* self,void* edx) {
    LONG64 t=qpcNow(); g_rwManagerOriginal(self,edx); rwPhaseRecord(RW_PARTICLES,t);
}
static void __fastcall hkRwPath(void* self,void* edx) {
    LONG64 t=qpcNow(); g_rwPathOriginal(self,edx); rwPhaseRecord(RW_PATH,t);
}
static DWORD WINAPI rwPhaseReport(void*) {
    LONG lastFrames=g_mkFrames; LONG64 previous[RW_PHASES]={};
    for (;;) {
        Sleep(5000); LONG frames=g_mkFrames, delta=frames-lastFrames; lastFrames=frames;
        for (int p=0;p<RW_PHASES;++p) {
            LONG64 ticks=rd64(&g_rwPhaseTicks[p]), change=ticks-previous[p]; previous[p]=ticks;
            char ms[32];
            if(delta>0)
                logf("rotwk-profile: %s inclusive %s ms/queued-Present, frames=%d calls=%d cumulative; nested phases overlap.",kRwPhaseName[p],msStr(change,delta,ms),delta,(int)g_rwPhaseCalls[p]);
            else
                logf("rotwk-profile: %s inclusive %s ms/report interval, queued-Presents=0; per-frame timing unavailable; calls=%d cumulative.",kRwPhaseName[p],msStr(change,1,ms),(int)g_rwPhaseCalls[p]);
        }
    }
}
static bool rwInstallPhaseTimers(DWORD hash,bool startReport=true) {
    if (!g_diag) return false;
    RwHook hooks[]={
        {0x6329B0,356,hash==0x88C193EE?0x1698D50Bu:0x60BF8EBDu,8,(void*)hkRwLogic},
        {0x632409,294,0x4D60CC97,5,(void*)hkRwClient},
        {0x5F5123,327,0x0698EE30,5,(void*)hkRwManager},
        {0x6F2364,666,0x4F58A019,6,(void*)hkRwPath}
    };
    if (!rwPrepareHooks(hooks,4)) return false;
    g_rwLogicOriginal=(tEngLogic)hooks[0].trampoline; g_rwClientOriginal=(tEngClient)hooks[1].trampoline;
    g_rwManagerOriginal=(tThis0)hooks[2].trampoline; g_rwPathOriginal=(tThis0)hooks[3].trampoline;
    if (!rwCommitHooks(hooks,4)) return false;
    if(startReport) { HANDLE h=CreateThread(NULL,0,rwPhaseReport,NULL,0,NULL); if(h) CloseHandle(h); }
    logf("rotwk-profile: coarse logic/client/particle/path timers LIVE at any FPS; legacy function map unused.");
    return true;
}
#endif
#include "aotr_rotwk_script.inc"
static void installRotwkWorkHooks(DWORD hash) {
    logf("rotwk-work: render-list %s; audio index %s.",rwInstallList()?"installed":"off",rwInstallAudio()?"installed":"off");
    logf("rotwk-work: script lookup %s.",rwInstallScriptLookup()?"installed":"off");
#ifndef AOTR_PROD
    if(g_diag && !rwInstallPhaseTimers(hash)) logf("rotwk-profile: target phase bodies differ; no phase hooks installed.");
#else
    (void)hash;
#endif
}
