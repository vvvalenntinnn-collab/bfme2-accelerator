// Run the shipping replacements against the user's privately mapped retail
// machine code. Never starts the game, modifies game.dat, or ships game bytes.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "aotr_accel.cpp"

static DWORD rng = 0x27A38951;
static DWORD randomWord() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float randomFloat() { return (int)(randomWord()%20001-10000) / 4096.0f; }
template<class T> static void put(void* p, unsigned offset, T value) { memcpy((BYTE*)p+offset,&value,sizeof(value)); }
static int runChild(const char* path,bool benchmark) {
    FILE* f = fopen(path,"rb"); assert(f);
    DWORD header[3]; assert(fread(header,4,3,f)==3 && header[0]==0x58465752);
    // Reserve before the child's loader places heaps in the low address range.
    char exe[MAX_PATH],command[MAX_PATH+16]; GetModuleFileNameA(NULL,exe,MAX_PATH);
    sprintf(command,"\"%s\" child%s",exe,benchmark ? " benchmark" : "");
    STARTUPINFOA startup={sizeof(startup)}; PROCESS_INFORMATION child={};
    assert(CreateProcessA(exe,command,NULL,NULL,TRUE,CREATE_SUSPENDED,NULL,NULL,&startup,&child));
    BYTE* image = (BYTE*)VirtualAllocEx(child.hProcess,(void*)0x400000,header[1],MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(image==(BYTE*)0x400000);
    for (DWORD i=0;i<header[2];++i) {
        DWORD section[2]; assert(fread(section,4,2,f)==2);
        assert(section[0]<=header[1] && section[1]<=header[1]-section[0]);
        std::vector<BYTE> bytes(section[1]);
        assert(fread(bytes.data(),1,section[1],f)==section[1]); SIZE_T written=0;
        assert(WriteProcessMemory(child.hProcess,image+section[0],bytes.data(),bytes.size(),&written) && written==bytes.size());
    }
    fclose(f);
    FlushInstructionCache(child.hProcess,image,header[1]);
    assert(ResumeThread(child.hThread)!=(DWORD)-1); CloseHandle(child.hThread);
    assert(WaitForSingleObject(child.hProcess,60000)==WAIT_OBJECT_0);
    DWORD code; assert(GetExitCodeProcess(child.hProcess,&code)); CloseHandle(child.hProcess);
    return (int)code;
}
static void* __cdecl retailAlloc(DWORD size,DWORD) { return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size); }
static void __cdecl retailFree(void* p) { assert(HeapFree(GetProcessHeap(),0,p)); }
static unsigned fallbackCalls;
static void __fastcall fallbackCall(void*,void*) { ++fallbackCalls; }

static void colors() {
    const DWORD special[] = {0,0x80000000,0x3F800000,0xBF800000,0x40000000,0x7F800000,
        0xFF800000,0x7FC12345,0xFFC34567,0x7FA56789,1,0x80000001,0x007FFFFF};
    std::vector<float> rgb(65536*3), alpha(65536), stock(65536*4+16), fast(stock.size());
    BYTE self[0x224]={}, buffers[3][24]={};
    put(buffers[0],12,stock.data()); put(buffers[1],12,rgb.data()); put(buffers[2],12,alpha.data());
    put(self,0x218,buffers[0]);
    unsigned cases=0;
    for (unsigned control=0;control<16;++control) for (unsigned mode=1;mode<4;++mode) {
        for (unsigned i=0;i<rgb.size();++i) { DWORD bits=special[randomWord()%13]; memcpy(&rgb[i],&bits,4); }
        for (unsigned i=0;i<alpha.size();++i) { DWORD bits=special[randomWord()%13]; memcpy(&alpha[i],&bits,4); }
        put(self,0x21C,mode&1 ? buffers[1] : (BYTE*)NULL);
        put(self,0x220,mode&2 ? buffers[2] : (BYTE*)NULL);
        const DWORD sizes[]={0,1,2,3,7,8,17,192,4097,65536};
        for (unsigned n:sizes) {
            put(self,0x100,n); std::fill(stock.begin(),stock.end(),-123.0f); fast=stock;
            DWORD csr=0x1F80|((control&3)<<13)|((control&4)?0x8000:0)|((control&8)?0x40:0);
            _mm_setcsr(csr); g_rwColorOriginal(self,NULL); DWORD stockCsr=_mm_getcsr();
            _mm_setcsr(csr); particleColorPack(fast.data(),mode&1?rgb.data():NULL,mode&2?alpha.data():NULL,n);
            assert(stockCsr==_mm_getcsr()); assert(!memcmp(stock.data(),fast.data(),stock.size()*4));
            ++cases;
        }
    }
    // Exercise the actual wrapper through proof, sampling and mismatch kill.
    put(self,0x100,17u); put(self,0x21C,buffers[1]); put(self,0x220,buffers[2]);
    for (int i=0;i<600;++i) hkRwParticleColor(self,NULL);
    assert(!g_rwColorKill && !g_rwColorProof);
    // Check conservative wrapper exits without executing unsupported layouts.
    RwColorFn original=g_rwColorOriginal; g_rwColorOriginal=fallbackCall; fallbackCalls=0;
    put(self,0x100,65537u); hkRwParticleColor(self,NULL);
    put(self,0x100,17u); put(buffers[0],12,rgb.data()); hkRwParticleColor(self,NULL); // alias
    put(buffers[0],12,stock.data());
    DWORD thread=g_mkTid; g_mkTid=0; hkRwParticleColor(self,NULL); g_mkTid=thread+1;
    hkRwParticleColor(self,NULL); g_mkTid=thread;
    _mm_setcsr(0x1F00); hkRwParticleColor(self,NULL); _mm_setcsr(0x1F80);
    assert(fallbackCalls==5); g_rwColorOriginal=original;
    // A deliberately incorrect reference leaves its output in place and kills
    // the replacement; do not let the proof path overwrite stock results.
    g_rwColorOriginal=fallbackCall; g_rwColorProof=1; std::fill(stock.begin(),stock.end(),-123.0f);
    hkRwParticleColor(self,NULL); assert(g_rwColorKill && stock[0]==-123.0f);
    g_rwColorOriginal=original; g_rwColorKill=false; g_rwColorProof=0;
    // Default-only release remains the actual stock path.
    put(self,0x21C,(BYTE*)NULL); put(self,0x220,(BYTE*)NULL); put(buffers[0],4,2u);
    hkRwParticleColor(self,NULL); assert(*(DWORD*)(buffers[0]+4)==1 && !*(void**)(self+0x218));
    _mm_setcsr(0x1F80);
    printf("PASS: %u native color cases, full capacity, guards, NaNs/zeros and MXCSR; live wrapper proofs\n",cases);
}

struct Child { void** vtable; DWORD id; };
struct Event { DWORD kind,id,arg,csr; DWORD matrix[12]; };
static std::vector<Event> events;
static BYTE* poseSelf;
static unsigned mutation;
static BYTE* pivotsBase;
static bool benchmarkEvents;
static BYTE* alternateTree;
static BYTE* initialTree;
static void record(unsigned kind,Child* child,DWORD arg,const float* matrix=NULL) {
    if (benchmarkEvents) return;
    Event e={}; e.kind=kind; e.id=child?child->id:0; e.arg=arg; e.csr=_mm_getcsr();
    if (matrix) memcpy(e.matrix,matrix,48); events.push_back(e);
}
static void __fastcall poseBase(void* self,void*) { assert(self==poseSelf); record(0,NULL,0); }
static void __fastcall childTransform(Child* child,void*,const float* matrix) {
    record(1,child,0,matrix);
    if (mutation&1) { float value=randomFloat(); put(pivotsBase,0x40,value); }
    if (mutation&2) _mm_setcsr((_mm_getcsr()&~0x6000u)|((child->id&3)<<13));
    if (mutation&4) _mm_setcsr(_mm_getcsr()&~63u);
    if (mutation&32) {
        BYTE* tree=*(BYTE**)(poseSelf+0xF8)==initialTree?alternateTree:initialTree;
        put(poseSelf,0xF8,tree); pivotsBase=*(BYTE**)(tree+0x14);
    }
}
static void __fastcall childHidden(Child* child,void*,DWORD hidden) {
    record(2,child,hidden);
    if (mutation&8) { float factor=randomFloat(); put(pivotsBase,0x54,factor); }
}
static void __fastcall childFactor(Child* child,void*,DWORD index,DWORD factor) {
    assert(!index); record(3,child,factor);
    if (mutation&16) pivotsBase[0x50]^=1;
}
static void __fastcall childUpdate(Child* child,void*) { record(4,child,0); }
static void* childMethods[128];
struct PoseFixture {
    BYTE self[0x180],tree[0x20],otherTree[0x20],pivots[130][0x58],otherPivots[130][0x58],groups[3][0x28];
    BYTE items[3][80][0x14],additional[80][0x14];
    Child children[320];
    void init(unsigned boneMode,unsigned count,unsigned seed) {
        memset(this,0,sizeof(*this)); rng=seed;
        put(self,0xF8,tree); put(tree,0x14,pivots); put(self,0x120,3u); put(self,0x128,groups);
        put(self,0x13C,additional); put(self,0x148,count); self[0x12]=0xFF;
        for (unsigned i=0;i<130;++i) {
            for (unsigned j=0;j<7;++j) { float f=randomFloat(); put(pivots[i],0x30+j*4,f); }
            pivots[i][0x50]=(BYTE)(i&1); put(pivots[i],0x54,randomFloat());
        }
        memcpy(otherPivots,pivots,sizeof(pivots));
        for (unsigned i=0;i<130;++i) put(otherPivots[i],0x40,randomFloat());
        put(otherTree,0x14,otherPivots); alternateTree=otherTree; initialTree=tree;
        for (unsigned lod=0;lod<3;++lod) {
            put(groups[lod],4,items[lod]); put(groups[lod],0x10,count);
            for (unsigned i=0;i<count;++i) {
                unsigned id=lod*80+i; children[id].vtable=childMethods; children[id].id=id;
                put(items[lod][i],0,&children[id]); put(items[lod][i],4,boneMode?i*2:0u);
            }
        }
        for (unsigned i=0;i<count;++i) {
            unsigned id=240+i; children[id].vtable=childMethods; children[id].id=id;
            put(additional[i],0,&children[id]); put(additional[i],4,boneMode?i*2:0u);
            for (unsigned j=0;j<3;++j) put(additional[i],8+j*4,i&1?randomFloat():0.0f);
        }
        poseSelf=self; pivotsBase=pivots[0];
    }
};
static void poses() {
    childMethods[0x54/4]=(void*)childTransform; childMethods[0x19C/4]=(void*)childHidden;
    childMethods[0x5C/4]=(void*)childFactor; childMethods[0xA8/4]=(void*)childUpdate;
    assert(patchJmp((BYTE*)0x5A5050,(void*)poseBase,6));
    g_rwPoseBase=poseBase;
    static PoseFixture f;
    const DWORD exceptional[]={0x7FC12345,0xFFC23456,0x7FA34567,0x7F800000,0xFF800000,1,0x80000000};
    unsigned cases=0;
    for (unsigned scenario=0;scenario<600;++scenario) {
        unsigned seed=randomWord(),boneMode=scenario&1,count=scenario%65,control=scenario%16;
        mutation=scenario%64;
        DWORD csr=0x1F80|((control&3)<<13)|((control&4)?0x8000:0)|((control&8)?0x40:0);
        f.init(boneMode,count,seed);
        if (scenario&2) for (unsigned i=0;i<7;++i) put(f.pivots[0],0x30+i*4,exceptional[(scenario+i)%7]);
        if (scenario&4) for (unsigned i=0;i<count;++i) put(f.additional[i],8+(i%3)*4,exceptional[(scenario+i)%7]);
        events.clear(); _mm_setcsr(csr); g_rwPoseOriginal(f.self,NULL);
        std::vector<Event> expected=events; DWORD finalCsr=_mm_getcsr(); BYTE flags=f.self[0x12];
        f.init(boneMode,count,seed);
        if (scenario&2) for (unsigned i=0;i<7;++i) put(f.pivots[0],0x30+i*4,exceptional[(scenario+i)%7]);
        if (scenario&4) for (unsigned i=0;i<count;++i) put(f.additional[i],8+(i%3)*4,exceptional[(scenario+i)%7]);
        events.clear(); _mm_setcsr(csr); hkRwPose(f.self,NULL);
        if (expected.size()!=events.size() || memcmp(expected.data(),events.data(),events.size()*sizeof(Event))) {
            for (unsigned i=0;i<events.size() && i<expected.size();++i) if (memcmp(&expected[i],&events[i],sizeof(Event))) {
                printf("POSE mismatch scenario %u event %u kind %u csr %x/%x\n",scenario,i,events[i].kind,expected[i].csr,events[i].csr);
                for (unsigned j=0;j<12;++j) if (expected[i].matrix[j]!=events[i].matrix[j])
                    printf("matrix[%u] %08x/%08x\n",j,expected[i].matrix[j],events[i].matrix[j]);
                break;
            }
            assert(false);
        }
        assert(finalCsr==_mm_getcsr() && flags==f.self[0x12]); ++cases;
    }
    _mm_setcsr(0x1F80);
    RwPoseFn original=g_rwPoseOriginal; g_rwPoseOriginal=fallbackCall; fallbackCalls=0;
    _mm_setcsr(0x1F00); hkRwPose(f.self,NULL); _mm_setcsr(0x1F80);
    assert(fallbackCalls==1); g_rwPoseOriginal=original;
    printf("PASS: %u native HLod cases: ordered callbacks, matrices, offsets, callback mutations, flags and MXCSR\n",cases);
}

struct System { BYTE bytes[0xB0]; };
struct Handle { System* system; void* prev; void* next; };
typedef void (__fastcall* PushFn)(void*,void*,const Handle*);
static PushFn pushNode=(PushFn)0x5F79BF;
static RwClearFn unlinkHandle=(RwClearFn)0x44C465;
static BYTE manager[0x60];
static BYTE* sentinel;
static System systems[600];
static void releaseHandle(Handle& h) { if (h.system) unlinkHandle(&h,NULL); }
static void append(System* system) { Handle h={system,NULL,NULL}; pushNode(manager+0x4C,NULL,&h); }
static void checkFind(DWORD id) {
    Handle a={},b={};
    DWORD ra=g_rwFindOriginal(manager,NULL,&a,id);
    DWORD rb=hkRwFind(manager,NULL,&b,id);
    assert(ra==(DWORD)&a && rb==(DWORD)&b && a.system==b.system);
    if (b.system) {
        // Stock weak-handle construction must register the returned observer.
        assert(*(Handle**)(b.system->bytes+0xA0)==&b);
        if (b.prev) assert(((Handle*)b.prev)->next==&b);
    } else assert(!b.prev && !b.next);
    releaseHandle(a); releaseHandle(b);
}
static void seedManager(unsigned count) {
    g_rwClearOriginal(manager+0x4C,NULL); rwIdInvalidate(); memset(systems,0,sizeof(systems));
    for (unsigned i=0;i<count;++i) { put(systems[i].bytes,0xA8,i+1); append(&systems[i]); }
}
static void warmIds() { for (unsigned i=0;i<520;++i) checkFind(100); }
static void __fastcall xferId(void*,void*,const char* name,void* value,DWORD size) {
    assert(size==4 && !strcmp(name,"ParticleSystemID")); *(DWORD*)value=901;
    for (unsigned i=0;i<12;++i) checkFind(100); // save/load callback cannot rebuild midway
    assert(!g_rwIds->ready);
}
static DWORD WINAPI foreignAssign(void*) {
    Handle h={}; g_rwAssignOriginal(&h,NULL,&h); // baseline works, then hook kills conservatively
    hkRwAssign(&h,NULL,&h); return 0;
}
static void ids() {
    assert(patchJmp((BYTE*)0x430130,(void*)retailAlloc,5));
    assert(patchJmp((BYTE*)0x430170,(void*)retailFree,5));
    sentinel=(BYTE*)retailAlloc(20,0); put(sentinel,0,sentinel); put(sentinel,4,sentinel);
    put(manager,0x4C,sentinel);
    seedManager(256); warmIds(); assert(g_rwIds->ready && !g_rwIdProof && !g_rwIdKill);
    rwIdInvalidate();for(unsigned i=0;i<8;++i) checkFind(100);assert(g_rwIds->ready);
    append(NULL);assert(g_rwIds->buildAfter==32);
    for(unsigned i=0;i<8;++i) checkFind(100);assert(!g_rwIds->ready && g_rwIds->queries==8);
    append(NULL);for(unsigned i=0;i<32;++i) checkFind(100);assert(g_rwIds->ready);
    append(NULL);assert(g_rwIds->buildAfter==8);warmIds();
    for (unsigned i=0;i<6000;++i) checkFind(randomWord()%400);
    // Duplicates retain the earliest node; misses and ID zero remain native.
    put(systems[256].bytes,0xA8,100u); append(&systems[256]); assert(!g_rwIds->ready); warmIds();
    assert(*(System**)(rwIdFind(100)+8)==&systems[99]);
    for (DWORD slot=0;slot<RW_ID_N;++slot) if (g_rwIds->ids[slot].node && g_rwIds->ids[slot].id==100) {
        g_rwIds->ids[slot].node=*(BYTE**)(sentinel+4); break; // deliberately pick the second duplicate
    }
    g_rwIdProof=1; checkFind(100); assert(g_rwIdKill); // caller still receives the stock first duplicate
    g_rwIdKill=0; g_rwIdProof=0; rwIdInvalidate(); warmIds();
    // Erase a live node, including null-valued nodes; do not use stale storage.
    BYTE* first=rwIdFind(100); void* next=NULL;
    ((RwEraseFn)0x5F5BC9)(manager+0x4C,NULL,&next,first); assert(!g_rwIds->ready);
    warmIds(); assert(*(System**)(rwIdFind(100)+8)==&systems[256]);
    // System destruction clears weak observers without assigning list nodes.
    // Reproduce those null writes, then confirm the hit check falls back safely.
    Handle* observer=*(Handle**)(systems[256].bytes+0x9C);
    while (observer) { Handle* after=(Handle*)observer->next; memset(observer,0,sizeof(*observer)); observer=after; }
    put(systems[256].bytes,0x9C,(Handle*)NULL); put(systems[256].bytes,0xA0,(Handle*)NULL);
    checkFind(100); assert(!g_rwIds->ready);
    append(&systems[256]); warmIds();
    append(NULL); warmIds(); BYTE* nullNode=*(BYTE**)(sentinel+4);
    ((RwEraseFn)0x5F5BC9)(manager+0x4C,NULL,&next,nullNode); assert(!g_rwIds->ready); warmIds();
    // Assignment into a list node invalidates even though the list links stay.
    BYTE* node=rwIdFind(100); Handle replacement={&systems[20],NULL,NULL};
    ((RwAssignFn)0x44C4E2)(node+8,NULL,&replacement); assert(!g_rwIds->ready); checkFind(100); checkFind(21);
    warmIds();
    void* methods[40]={}; methods[0x94/4]=(void*)xferId; void** object=methods;
    ((RwXferFn)0x7077DF)(&object,systems[20].bytes+0xA8);
    assert(!g_rwIds->ready && !g_rwIdMutationDepth); checkFind(901); checkFind(21);
    ((RwClearFn)0x5F5DAF)(manager+0x4C,NULL); assert(!g_rwIds->ready); checkFind(901);
    seedManager(16); for (unsigned i=0;i<32;++i) checkFind(i%16+1); assert(!g_rwIds->ready);
    // Deliberately exhaust one probe cluster; preserve answers via stock walk.
    seedManager(0); DWORD key=1,lastKey=0;
    for (unsigned i=0;i<33;++i) {
        while (rwIdHash(key)!=0) ++key;
        lastKey=key++; put(systems[i].bytes,0xA8,lastKey); append(&systems[i]);
    }
    for (unsigned i=0;i<16;++i) checkFind(lastKey);
    assert(!g_rwIds->ready && g_rwIds->queries==8);
    seedManager(0); put(systems[0].bytes,0xA8,1u);
    for (unsigned i=0;i<4097;++i) append(&systems[0]);
    for (unsigned i=0;i<16;++i) checkFind(1);
    assert(!g_rwIds->ready && g_rwIds->queries==8);
    seedManager(256); warmIds();
    HANDLE thread=CreateThread(NULL,0,foreignAssign,NULL,0,NULL);
    assert(thread && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CloseHandle(thread);
    assert(g_rwIdKill); checkFind(100);
    g_rwClearOriginal(manager+0x4C,NULL); retailFree(sentinel);
    printf("PASS: native ID lookup/observer links, duplicates/misses, collisions/capacity, insert/erase/null erase/clear/assignment/save-load and foreign-thread fallback\n");
}
template<class Fn> static double timed(unsigned iterations,Fn fn) {
    LARGE_INTEGER hz,start,end; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&start);
    for (unsigned i=0;i<iterations;++i) fn(i);
    QueryPerformanceCounter(&end); return (double)(end.QuadPart-start.QuadPart)*1e9/hz.QuadPart/iterations;
}
template<class A,class B> static void compareBench(const char* name,unsigned iterations,A native,B fast) {
    double a[7],b[7];
    for (unsigned i=0;i<7;++i) {
        if (i&1) { b[i]=timed(iterations,fast); a[i]=timed(iterations,native); }
        else { a[i]=timed(iterations,native); b[i]=timed(iterations,fast); }
    }
    std::sort(a,a+7); std::sort(b,b+7);
    printf("BENCH %s: native %.2f ns, accelerated %.2f ns, %.2fx (seven-sample median)\n",name,a[3],b[3],a[3]/b[3]);
}
static volatile DWORD benchSink;
static void benchmarks() {
    DWORD_PTR affinity,systemAffinity; GetProcessAffinityMask(GetCurrentProcess(),&affinity,&systemAffinity);
    assert(SetThreadAffinityMask(GetCurrentThread(),affinity & (0-affinity)));
    _mm_setcsr(0x1F80);
    const unsigned sizes[]={192,4096,65536};
    for (unsigned n:sizes) {
        BYTE self[0x224]={},buffers[3][24]={};
        std::vector<float> rgb(n*3),alpha(n),out(n*4);
        for (unsigned i=0;i<rgb.size();++i) rgb[i]=randomFloat();
        for (unsigned i=0;i<n;++i) alpha[i]=randomFloat();
        put(buffers[0],12,out.data()); put(buffers[1],12,rgb.data()); put(buffers[2],12,alpha.data());
        put(self,0x100,n); put(self,0x218,buffers[0]); put(self,0x21C,buffers[1]); put(self,0x220,buffers[2]);
        char name[64]; sprintf(name,"particle color capacity %u",n);
        compareBench(name,n<1000?10000:500,[&](unsigned){g_rwColorOriginal(self,NULL);},
            [&](unsigned){hkRwParticleColor(self,NULL);});
        benchSink=*(DWORD*)out.data();
    }
    benchmarkEvents=true; mutation=0;
    static PoseFixture pose;
    for (unsigned unique=0;unique<2;++unique) {
        pose.init(unique,64,0x71356149);
        compareBench(unique?"HLod 256 children, 64 different bones":"HLod 256 children, shared bone",10000,
            [&](unsigned){g_rwPoseOriginal(pose.self,NULL);},[&](unsigned){hkRwPose(pose.self,NULL);});
    }
    benchmarkEvents=false;
    // Fresh real native lists/observer links, with the live sampled fast path.
    g_rwIdKill=0; g_rwIdProof=0;
    sentinel=(BYTE*)retailAlloc(20,0); put(sentinel,0,sentinel); put(sentinel,4,sentinel);
    put(manager,0x4C,sentinel); rwIdInvalidate();
    for (unsigned count:{64u,256u,512u}) {
        seedManager(count); for (unsigned i=0;i<8;++i) checkFind(count);
        char name[64]; sprintf(name,"particle ID hit, %u systems",count);
        auto lookup=[&](RwFindFn fn,unsigned i) {
            Handle h={}; fn(manager,NULL,&h,(i%count)+1); benchSink=(DWORD)h.system; releaseHandle(h);
        };
        compareBench(name,100000,[&](unsigned i){lookup(g_rwFindOriginal,i);},[&](unsigned i){lookup(hkRwFind,i);});
    }
    for(unsigned queries:{1u,8u,32u,128u}) {
        seedManager(256);g_rwIdProof=0;char name[96];sprintf(name,"particle ID churn, 256 systems, %u lookups per erase/insert",queries);
        auto cycle=[&](RwFindFn function,unsigned iteration) {
            BYTE* last=*(BYTE**)(sentinel+4);void* after=NULL;
            ((RwEraseFn)0x5F5BC9)(manager+0x4C,NULL,&after,last);append(&systems[255]);
            for(unsigned q=0;q<queries;++q) {
                Handle handle={};DWORD id=(iteration*37+q*17)%256+1;
                function(manager,NULL,&handle,id);benchSink=(DWORD)handle.system;assert(handle.system==&systems[id-1]);releaseHandle(handle);
            }
        };
        compareBench(name,2000,[&](unsigned i){cycle(g_rwFindOriginal,i);},[&](unsigned i){cycle(hkRwFind,i);});
    }
    g_rwClearOriginal(manager+0x4C,NULL); retailFree(sentinel);
    assert(SetThreadAffinityMask(GetCurrentThread(),affinity));
}
static unsigned failStep,patchStep;
static BOOL failPatch(BYTE* target,void* dest,int stolen) { return patchStep++==failStep ? FALSE : patchJmp(target,dest,stolen); }
static void transactions() {
    // Use independent private mapped addresses. Check rollback after each
    // possible partial commit, and complete-body rejection before any patch.
    RwHook hooks[]={
        {0x5F5487,34,0x6B01CE05,5,(void*)hkRwNode},
        {0x5F5BC9,42,0x1C2E8E2B,6,(void*)hkRwErase},
        {0x5F5DAF,51,0x78444B01,6,(void*)hkRwClear},
        {0x44C4E2,44,0xB7E77D2B,6,(void*)hkRwAssign},
        {0x7077DF,24,0xD86B267D,6,(void*)hkRwXfer},
        {0x5F530A,111,0x219D8714,5,(void*)hkRwFind}
    };
    assert(rwPrepareHooks(hooks,6));
    for (failStep=0;failStep<6;++failStep) {
        patchStep=0; assert(!rwCommitHooks(hooks,6,failPatch));
        assert(rwBodiesMatch(hooks,6));
    }
    BYTE* p=(BYTE*)hooks[0].address+12; BYTE saved=*p; *p^=1;
    assert(!rwPrepareHooks(hooks,6)); *p=saved; assert(rwBodiesMatch(hooks,6));
    g_rwIdKill=0;
    printf("PASS: six partial-install failures rolled back and full-body mismatch rejected\n");
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2 || argc==3); if (strcmp(argv[1],"child")) return runChild(argv[1],argc==3);
    InitializeCriticalSection(&g_logCs); g_mkTid=GetCurrentThreadId();
    events.reserve(1600); transactions();
    SetEnvironmentVariableA("AOTR_PARTICLECOLOR","0"); SetEnvironmentVariableA("AOTR_PARTICLEINDEX","0");
    SetEnvironmentVariableA("AOTR_POSEMATRIX",NULL); installRotwkFxHooks();
    assert(*(BYTE*)0x5A7DF0!=0xE9 && *(BYTE*)0x59CD70!=0xE9 && *(BYTE*)0x5F530A!=0xE9);
    assert(!g_rwColorScratch && !g_rwIds && !g_rwPoseOriginal);
    SetEnvironmentVariableA("AOTR_PARTICLECOLOR",NULL); SetEnvironmentVariableA("AOTR_PARTICLEINDEX",NULL);
    SetEnvironmentVariableA("AOTR_POSEMATRIX","1"); installRotwkFxHooks();
    assert(*(BYTE*)0x5A7DF0==0xE9 && *(BYTE*)0x59CD70==0xE9 && *(BYTE*)0x5F530A==0xE9);
    colors(); poses(); ids(); if (argc==3) benchmarks(); DeleteCriticalSection(&g_logCs); return 0;
}
