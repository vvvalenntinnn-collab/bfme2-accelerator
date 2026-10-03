// PointGroup APT compression, independently mapped from the BFME2 reference
// (BFME2 RVA 17F1B0 -> supplied ROTWK RVA 17EA80). Stock owns resize/defaults.
typedef void (__fastcall* RwGatherFn)(void*,void*,void**,void**,void**,void**,void**);
static RwGatherFn g_rwGatherOriginal=NULL;
static BYTE* g_rwGatherScratch=NULL;
static DWORD g_rwGatherProof=512,g_rwGatherTick=0;
static bool g_rwGatherKill=false;
static const DWORD kRwGatherVector[5]={0xDDEF10,0xDDEEF0,0xD9B540,0xD9B550,0xD9B560};
static const DWORD kRwGatherOffset[5]={4,8,0x10,0x14,0x18},kRwGatherWidth[5]={12,16,4,1,1};
struct RwGatherJob { const DWORD* apt; const BYTE* src[5]; BYTE* dst[5]; };
static void rwGatherRange(void* context,unsigned begin,unsigned end) {
    RwGatherJob& j=*(RwGatherJob*)context;
    for(unsigned i=begin;i<end;++i) {
        DWORD index=j.apt[i];
        if(j.src[0]) {
            const BYTE* p=j.src[0]+index*12; BYTE* d=j.dst[0]+i*12;
            _mm_storel_epi64((__m128i*)d,_mm_loadl_epi64((const __m128i*)p)); memcpy(d+8,p+8,4);
        }
        if(j.src[1]) _mm_storeu_si128((__m128i*)(j.dst[1]+i*16),_mm_loadu_si128((const __m128i*)(j.src[1]+index*16)));
        if(j.src[2]) memcpy(j.dst[2]+i*4,j.src[2]+index*4,4);
        if(j.src[3]) j.dst[3][i]=j.src[3][index];
        if(j.src[4]) j.dst[4][i]=j.src[4][index];
    }
}
static bool rwGatherLayout(BYTE* self,void*** outputs,RwGatherJob* job,DWORD* count) {
    DWORD n=*(DWORD*)(self+0x1C); BYTE* apt=*(BYTE**)(self+0xC);
    if(!apt || n<32 || n>RW_COLOR_MAX || *(DWORD*)(apt+16)<n) return false;
    job->apt=*(const DWORD**)(apt+12); if(!job->apt) return false;
    DWORD capacity[5]={},limit=0xFFFFFFFF;
    for(unsigned f=0;f<5;++f) {
        BYTE* source=*(BYTE**)(self+kRwGatherOffset[f]); DWORD* v=(DWORD*)kRwGatherVector[f];
        job->src[f]=source?*(const BYTE**)(source+12):NULL; job->dst[f]=source?(BYTE*)v[1]:NULL;
        if(source) {
            capacity[f]=*(DWORD*)(source+16);
            if(!job->src[f] || !job->dst[f] || (LONG)v[2]<(LONG)n || capacity[f]>1048576) return false;
            if(capacity[f]<limit) limit=capacity[f];
        }
        for(unsigned o=0;o<5;++o) {
            if(!outputs[o] || rwOverlap(outputs[o],4,self,0x38) || rwOverlap(outputs[o],4,apt,24) ||
                rwOverlap(outputs[o],4,v,16) || (source && rwOverlap(outputs[o],4,source,24))) return false;
        }
    }
    // Reordering field copies is valid only for completely disjoint output,
    // input and out-parameter storage. Resize and unusual aliases stay stock.
    for(unsigned f=0;f<5;++f) if(job->dst[f]) {
        DWORD bytes=n*kRwGatherWidth[f];
        if(rwOverlap(job->dst[f],bytes,job->apt,n*4)) return false;
        for(unsigned q=0;q<5;++q) {
            if(rwOverlap(job->dst[f],bytes,job->src[q],capacity[q]*kRwGatherWidth[q]) ||
                (q!=f && rwOverlap(job->dst[f],bytes,job->dst[q],n*kRwGatherWidth[q])) ||
                rwOverlap(outputs[q],4,job->dst[f],bytes) ||
                rwOverlap(outputs[q],4,job->src[f],capacity[f]*kRwGatherWidth[f]) ||
                rwOverlap(outputs[q],4,job->apt,n*4)) return false;
        }
    }
    for(DWORD i=0;i<n;++i) if(job->apt[i]>=limit) return false;
    *count=n; return true;
}
static void __fastcall hkRwGather(void* object,void* edx,void** loc,void** diffuse,void** size,void** orient,void** frame) {
    if(g_rwGatherKill || !g_mkTid || GetCurrentThreadId()!=g_mkTid) {
        g_rwGatherOriginal(object,edx,loc,diffuse,size,orient,frame); return;
    }
    void** outputs[]={loc,diffuse,size,orient,frame}; RwGatherJob job={}; DWORD n;
    if(!rwGatherLayout((BYTE*)object,outputs,&job,&n)) {
        g_rwGatherOriginal(object,edx,loc,diffuse,size,orient,frame); return;
    }
    bool check=g_rwGatherProof || ((++g_rwGatherTick&63)==0);
    if(check) {
        RwGatherJob scratch=job; DWORD offset=0;
        for(unsigned f=0;f<5;++f) { scratch.dst[f]=job.src[f]?g_rwGatherScratch+offset:NULL;offset+=n*kRwGatherWidth[f]; }
        if(!rwRunPreparation(rwGatherRange,&scratch,n)) rwGatherRange(&scratch,0,n);
        g_rwGatherOriginal(object,edx,loc,diffuse,size,orient,frame);
        bool equal=true;
        for(unsigned f=0;f<5;++f)
            if(*outputs[f]!=job.dst[f] || (job.src[f] && memcmp(*outputs[f],scratch.dst[f],n*kRwGatherWidth[f]))) equal=false;
        if(!equal) { g_rwGatherKill=true;logf("rotwk: selected-particle gather mismatch - fused gather OFF."); }
        else if(g_rwGatherProof && !--g_rwGatherProof) logf("rotwk: 512 selected-particle gathers matched; fused gather live (1 in 64 checked).");
        return;
    }
    if(!rwRunPreparation(rwGatherRange,&job,n)) rwGatherRange(&job,0,n);
    for(unsigned f=0;f<5;++f) *outputs[f]=job.dst[f];
}
static void installRotwkGather() {
    if(!rwOptIn("AOTR_PARTICLEGATHER","PARTICLE_GATHER_ON")) return;
    const RwHook dependencies[]={
        {0xB318F0,249,0x9DE80AFB},{0xB319F0,277,0x3AA2D946},{0xB31B50,155,0x81BF8843},{0xB31B10,52,0x184C7247}
    };
    RwHook hook={0x57EA80,488,0x8A2D9ECE,6,(void*)hkRwGather};
    g_rwGatherScratch=(BYTE*)VirtualAlloc(NULL,RW_COLOR_MAX*34,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    bool ok=g_rwGatherScratch && rwBodiesMatch(dependencies,4) && rwPrepareHooks(&hook,1);
    if(ok) { g_rwGatherOriginal=(RwGatherFn)hook.trampoline; ok=rwCommitHooks(&hook,1); }
    logf("rotwk: experimental selected-particle fused gather %s; native allocation/default branches retained.",ok?"hooked":"OFF");
}
