// The retail findScript wrapper already selects a side and uses an indexed
// table. Keep that wrapper's string ownership/prefix/output ABI native; fuse
// only its ScriptList lookup. No name/pointer cache survives a call.
typedef void* (__fastcall* RwScriptFn)(void*,void*,const DWORD*);
static RwScriptFn g_rwScriptOriginal=NULL;
static DWORD g_rwScriptProof=512,g_rwScriptTick=0;
static bool g_rwScriptKill=false;
static const RwRegion kRwScriptRegions[]={
    {0x00604A5D,128,0xD338F3DD}, // qualified findScript wrapper, unchanged
    {0x007B72EC,41,0xCFD191AD},  // ScriptList's second-table lookup
    {0x007B712D,67,0xA4310078},  // search then reverse equality check
    {0x007B6287,80,0xD4DE2712},  // midpoint-on-equality binary search
    {0x004065AA,42,0x2486D6F6},  // StringBase comparison
    {0x00406307,63,0x08FD6040},
    {0x004052F9,43,0x3FE1424B}   // memcmp(min(lengths)), then length difference
};
static bool rwScriptCandidate(void* object,const DWORD* name,void** result) {
    if(!object || !name) return false;
    const BYTE* table=(const BYTE*)object+0x2C;
    DWORD begin=*(const DWORD*)table,end=*(const DWORD*)(table+4);
    if(end<begin || ((end-begin)&3) || end-begin>(1u<<22)) return false;
    DWORD count=(end-begin)/4;
    if(!count) { *result=NULL; return true; }
    DWORD records=*(const DWORD*)(table+12),recordEnd=*(const DWORD*)(table+16);
    if(!begin || !records || recordEnd<records || (recordEnd-records)%20) return false;
    DWORD recordCount=(recordEnd-records)/20;
    DWORD key=*name;
    unsigned keyLength=key?*(const WORD*)(key+4):0;
    const BYTE* keyBytes=key?(const BYTE*)(key+8):NULL;
    DWORD lo=0,hi=count;
    while(lo<hi) {
        DWORD mid=(lo+hi)/2;
        DWORD id=((const DWORD*)begin)[mid];
        if(id>=recordCount) return false;
        const BYTE* record=(const BYTE*)records+id*20;
        DWORD buffer=*(const DWORD*)(record+8);
        unsigned length=buffer?*(const WORD*)(buffer+4):0;
        unsigned common=keyLength<length?keyLength:length;
        // Embedded NULs participate: retail calls memcmp, not strcmp/strncmp.
        // Reuse the accelerator's length-bounded SSE2 comparison. A compiler
        // memcmp intrinsic can emit REP CMPS and lose on shared prefixes.
        int cmp=common?fastMemcmp(keyBytes,(const BYTE*)(buffer+8),common):0;
        if(!cmp) cmp=(int)keyLength-(int)length;
        if(!cmp) { *result=(void*)(*(const DWORD*)(record+16)+4); return true; }
        if(cmp<0) hi=mid; else lo=mid+1;
    }
    *result=NULL; return true;
}
static void* __fastcall hkRwScriptLookup(void* object,void* edx,const DWORD* name) {
    if(!g_mkTid || GetCurrentThreadId()!=g_mkTid || g_rwScriptKill)
        return g_rwScriptOriginal(object,edx,name);
    void* answer;
    if(!rwScriptCandidate(object,name,&answer)) return g_rwScriptOriginal(object,edx,name);
    if(g_rwScriptProof || ((++g_rwScriptTick&63)==0)) {
        void* stock=g_rwScriptOriginal(object,edx,name);
        if(answer!=stock) {
            g_rwScriptKill=true;
            logf("rotwk: script lookup mismatch - fused search OFF for this session.");
        } else if(g_rwScriptProof && !--g_rwScriptProof)
            logf("rotwk: 512 script lookup results matched; fused search live (1 in 64 checked).");
        return stock;
    }
    return answer;
}
static bool rwInstallScriptLookup() {
    if(!rwEnabled("AOTR_SCRIPTLOOKUP") || !rwRegionsMatch(kRwScriptRegions,_countof(kRwScriptRegions))) return false;
    RwHook hook={0x007B72EC,41,0xCFD191AD,5,(void*)hkRwScriptLookup};
    if(!rwPrepareHooks(&hook,1)) return false;
    g_rwScriptOriginal=(RwScriptFn)hook.trampoline;
    RwEdit edit=rwBranchEdit(hook.address,hook.stolen,0xE9,hook.replacement);
    if(!rwCommitEdits(&edit,1,kRwScriptRegions,_countof(kRwScriptRegions))) return false;
    logf("rotwk: ScriptList search installed; qualified findScript/ownership remain native; no persistent lookup cache.");
    return true;
}
