// Experimental whole-batch measurements, never installed into the game.
// Native raw-animation path only; fabricated, immutable clip data and owned trees.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <xmmintrin.h>
#include "aotr_prepare_pool.h"
#include "aotr_particle_math.h"
#include "mapped_retail_test.h"

static LARGE_INTEGER frequency;
static volatile DWORD sink;
static LONG64 now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
template<class T> static void put(void* p,unsigned offset,T value) { memcpy((BYTE*)p+offset,&value,sizeof(value)); }
static DWORD hash(const void* p,unsigned n) {
    DWORD h=2166136261u; const BYTE* b=(const BYTE*)p;
    for(unsigned i=0;i<n;++i) h=(h^b[i])*16777619u;
    return h;
}
template<class A,class B> static void bench(const char* name,unsigned iterations,A serial,B batch) {
    double a[7],b[7];
    auto measure=[&](auto fn) {
        LONG64 start=now(); for(unsigned i=0;i<iterations;++i) fn();
        return (double)(now()-start)*1e6/frequency.QuadPart/iterations;
    };
    serial(); batch();
    for(unsigned i=0;i<7;++i) {
        if(i&1) { b[i]=measure(batch);a[i]=measure(serial); }
        else { a[i]=measure(serial);b[i]=measure(batch); }
    }
    std::sort(a,a+7);std::sort(b,b+7);
    printf("BENCH %s: serial %.3f us, batch %.3f us, %.3fx (seven-sample median)\n",name,a[3],b[3],a[3]/b[3]);
}

struct ColorJob { float* out; const float* rgb; const float* alpha; unsigned count,end; };
struct ColorBatch { const ColorJob* jobs; unsigned size; };
// Flatten the batch into point ranges. One wake/join covers all systems;
// a chunk can straddle several small buffers without touching their tails.
static void batchColorRange(void* ptr,unsigned begin,unsigned end) {
    const ColorBatch& b=*(ColorBatch*)ptr;
    unsigned low=0,high=b.size;
    while(low<high) { unsigned mid=(low+high)/2;if(b.jobs[mid].end<=begin) low=mid+1;else high=mid; }
    for(unsigned i=low;begin<end;++i) {
        const ColorJob& j=b.jobs[i];unsigned start=j.end-j.count,offset=begin-start;
        unsigned last=(std::min)(end,j.end),n=last-begin;
        particleColorPack(j.out+offset*4,j.rgb?j.rgb+offset*3:NULL,j.alpha?j.alpha+offset:NULL,n);
        begin=last;
    }
}
static void runColors(AccelPreparePool& pool,std::vector<ColorJob>& jobs) {
    // Include descriptor preparation in the timed path. Borrowed buffers must
    // remain immutable and outputs must not alias any other job's buffers.
    unsigned total=0;
    for(auto& j:jobs) { assert(j.count<=65536 && total<=(1u<<24)-j.count);total+=j.count;j.end=total; }
    ColorBatch batch={jobs.data(),(unsigned)jobs.size()};
    if(total>=32768 && pool.run(batchColorRange,&batch,total,2048)) return;
    for(const auto& j:jobs) particleColorPack(j.out,j.rgb,j.alpha,j.count);
}

typedef void (__fastcall* NativePose)(void*,void*,const float*,void*,float);
static NativePose nativePose=(NativePose)0x563310;
// This fixture's vtable reads only its own immutable fields. These are NOT
// claims about every concrete animation class used by the real game.
static int __fastcall clipFrames(void* clip,void*) { return *(int*)((BYTE*)clip+0x48); }
static int __fastcall clipBones(void* clip,void*) { return *(int*)((BYTE*)clip+0x4C); }
struct Clip {
    BYTE* memory;unsigned bytes,bones,frames;
    Clip(unsigned n):memory(NULL),bytes(0),bones(n),frames(16) {
        bytes=4096+(((n*36+n*32*2+n*frames*5*4)+4095)&~4095u);
        memory=(BYTE*)VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(memory);
        DWORD* vt=(DWORD*)memory;BYTE* clip=memory+256;BYTE* nodes=memory+4096;
        BYTE* channels=nodes+n*36;float* data=(float*)(channels+n*64);
        vt[0x14/4]=(DWORD)clipFrames;vt[0x34/4]=(DWORD)clipBones;
        put(clip,0,vt);put(clip,0x48,frames);put(clip,0x4C,n);put(clip,0x50,nodes);
        for(unsigned i=0;i<n;++i) {
            BYTE* x=channels+i*64;BYTE* q=x+32;
            float* translations=data+i*frames*5;float* rotations=translations+frames;
            put(x,4,0u);put(x,8,1u);put(x,0x14,translations);put(x,0x18,0u);put(x,0x1C,frames-1);
            put(q,4,6u);put(q,8,4u);put(q,0x14,rotations);put(q,0x18,0u);put(q,0x1C,frames-1);
            put(nodes+i*36,0,x);put(nodes+i*36,0x18,q);
            for(unsigned f=0;f<frames;++f) {
                translations[f]=(float)((int)((i*7+f*3)%31)-15)/128.0f;
                // Exact unit quaternions; changing axes exercises the hierarchy.
                rotations[f*4+(i+f)%4]=1.0f;
            }
        }
        DWORD old;assert(VirtualProtect(memory,bytes,PAGE_READONLY,&old));
    }
    void* object() const { return memory+256; }
    ~Clip() { VirtualFree(memory,0,MEM_RELEASE); }
};
struct Skeleton {
    BYTE tree[40];std::vector<BYTE> pivots;float root[12];float frame;
    Skeleton(unsigned n,unsigned index):pivots(n*88),frame(0) {
        memset(tree,0,sizeof(tree));memset(root,0,sizeof(root));root[0]=root[5]=root[10]=1.0f;
        root[3]=(float)(index%37);root[7]=(float)(index%19);root[11]=(float)(index%7);
        put(tree,0x10,n);put(tree,0x14,pivots.data());put(tree,0x18,index&1?0.75f:1.0f);
        for(unsigned i=0;i<n;++i) {
            BYTE* p=pivots.data()+i*88;
            if(i) put(p,0x10,pivots.data()+((i-1)/2)*88);
            put(p,0x20,1.0f); // Base quaternion (x,y,z,w) at +0x14.
            put(p,0x24,(float)(i%3)/16.0f);put(p,0x28,(float)(i%5)/32.0f);
            put(p,0x4C,i); // Non-captured bone; capture list is empty.
        }
    }
};
struct PoseBatch { Skeleton** jobs;unsigned count;void* clip; };
static void poseRange(void* ptr,unsigned begin,unsigned end) {
    PoseBatch& b=*(PoseBatch*)ptr;
    for(unsigned i=begin;i<end;++i) {
        Skeleton& s=*b.jobs[i];nativePose(s.tree,NULL,s.root,b.clip,s.frame);
    }
}

// Separate experimental pool: the shipping SSE-only pool must NOT invoke
// native x87 code or arbitrary engine callbacks. Preserve each worker's x87
// environment and join masked exception flags, including frame conversion.
struct FpEnv { WORD control,pad0,status,pad1,tag,pad2;DWORD words[4]; };
static_assert(sizeof(FpEnv)==28,"x86 x87 environment");
static FpEnv saveEnv() { FpEnv e;__asm fnstenv e
    __asm fldenv e
    return e;
}
static void restoreEnv(const FpEnv& e) {
    const FpEnv* p=&e;
    __asm {
        mov eax,p
        fldenv [eax]
    }
}
static WORD fpStatus() { WORD s;__asm fnstsw s
    return s;
}
struct NativePool {
    HANDLE threads[2],wake[2],done[2];struct Slot { NativePool* p;unsigned i; } slots[2];
    unsigned workers,count,grain,csr;WORD control;DWORD sseStatus[2];WORD x87Status[2];
    volatile LONG next,stop;PoseBatch* batch;
    void consume() {
        for(;;) { unsigned first=(unsigned)InterlockedExchangeAdd(&next,grain);if(first>=count) return;
            poseRange(batch,first,(std::min)(count,first+grain)); }
    }
    static DWORD WINAPI entry(void* ptr) {
        Slot& s=*(Slot*)ptr;NativePool& p=*s.p;
        for(;;) {
            assert(WaitForSingleObject(p.wake[s.i],INFINITE)==WAIT_OBJECT_0);if(p.stop) return 0;
            FpEnv saved=saveEnv();assert(saved.tag==0xFFFF);DWORD savedCsr=_mm_getcsr();WORD cw=p.control;
            __asm fnclex
            __asm fldcw cw
            _mm_setcsr(p.csr);p.consume();p.x87Status[s.i]=fpStatus()&63;p.sseStatus[s.i]=_mm_getcsr()&63;
            restoreEnv(saved);_mm_setcsr(savedCsr);assert(SetEvent(p.done[s.i]));
        }
    }
    void start(const AccelCpuPlan& plan) {
        workers=(std::min)(2u,plan.count>2?(unsigned)plan.count-2:0u);
        for(unsigned i=0;i<workers;++i) {
            wake[i]=CreateEventA(NULL,FALSE,FALSE,NULL);done[i]=CreateEventA(NULL,FALSE,FALSE,NULL);slots[i]={this,i};
            threads[i]=CreateThread(NULL,0,entry,&slots[i],0,NULL);assert(wake[i]&&done[i]&&threads[i]);
            assert(SetThreadAffinityMask(threads[i],(DWORD_PTR)1<<plan.cpu[i+2]));
        }
    }
    bool run(PoseBatch& b) {
        FpEnv e=saveEnv();
        if(!workers || b.count<32 || (e.control&63)!=63 || e.tag!=0xFFFF || (_mm_getcsr()&0x1F80)!=0x1F80) return false;
        batch=&b;count=b.count;grain=8;next=0;control=e.control;csr=_mm_getcsr();
        for(unsigned i=0;i<workers;++i) {x87Status[i]=0;sseStatus[i]=0;assert(SetEvent(wake[i]));}
        consume();assert(WaitForMultipleObjects(workers,done,TRUE,INFINITE)==WAIT_OBJECT_0);
        e=saveEnv();DWORD finalCsr=_mm_getcsr();
        for(unsigned i=0;i<workers;++i) { e.status|=x87Status[i];finalCsr|=sseStatus[i]; }
        restoreEnv(e);_mm_setcsr(finalCsr);batch=NULL;return true;
    }
    void shutdown() {
        stop=1;for(unsigned i=0;i<workers;++i) assert(SetEvent(wake[i]));
        for(unsigned i=0;i<workers;++i) {
            assert(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);
            CloseHandle(threads[i]);CloseHandle(wake[i]);CloseHandle(done[i]);
        }
        workers=0;stop=0;
    }
};
static void runPoses(NativePool& pool,PoseBatch& b) { if(!pool.run(b)) poseRange(&b,0,b.count); }
struct Army {
    std::vector<Skeleton*> a,b;Clip clip;
    Army(unsigned count,unsigned bones):clip(bones) {
        for(unsigned i=0;i<count;++i) {a.push_back(new Skeleton(bones,i));b.push_back(new Skeleton(bones,i));}
    }
    ~Army() {for(auto s:a) delete s;for(auto s:b) delete s;}
    void compare() {
        for(unsigned i=0;i<a.size();++i) for(unsigned bone=0;bone<clip.bones;++bone) {
            const BYTE* x=a[i]->pivots.data()+bone*88;const BYTE* y=b[i]->pivots.data()+bone*88;
            assert(!memcmp(x+0x30,y+0x30,40));assert(!memcmp(x,y,16));assert(!memcmp(x+0x14,y+0x14,28));
        }
    }
};
static void correctness(AccelPreparePool& colorPool,NativePool& posePool) {
    unsigned colors=0,poses=0;
    // Irregular systems, empty buffers, all optional inputs, chunk boundaries
    // and out-of-range colors/NaNs/signed zeros; sentinel tails are compared.
    for(unsigned mode=0;mode<4;++mode) {
        std::vector<std::vector<float>> rgb(79),alpha(79),a(79),b(79);std::vector<ColorJob> jobs;
        unsigned total=0;
        for(unsigned i=0;i<79;++i) {
            unsigned n=i%5?17+(i*379)%4097:0;total+=n;
            rgb[i].resize(n*3);alpha[i].resize(n);a[i].resize(n*4+4,123.0f);b[i]=a[i];
            for(unsigned k=0;k<n*3;++k) { DWORD bits[]={0x80000000,0x7FC12345,0x3F800000,0xBF800000,0x40000000,0x3ECCCCCD};put(rgb[i].data(),k*4,bits[k%6]); }
            for(unsigned k=0;k<n;++k) alpha[i][k]=(float)((int)(k%7)-2)*0.4f;
            ColorJob j={b[i].data(),mode&1?rgb[i].data():NULL,mode&2?alpha[i].data():NULL,n,0};jobs.push_back(j);
        }
        for(unsigned rounding=0;rounding<4;++rounding) {
            DWORD csr=0x1F80|(rounding<<13);_mm_setcsr(csr);
            for(unsigned i=0;i<jobs.size();++i) particleColorPack(a[i].data(),jobs[i].rgb,jobs[i].alpha,jobs[i].count);
            DWORD expected=_mm_getcsr();_mm_setcsr(csr);runColors(colorPool,jobs);
            assert(_mm_getcsr()==expected);for(unsigned i=0;i<79;++i) assert(!memcmp(a[i].data(),b[i].data(),a[i].size()*4));
            colors+=total;
        }
    }
    _mm_setcsr(0x1F80);Army army(79,48);PoseBatch a={army.a.data(),79,army.clip.object()},b={army.b.data(),79,army.clip.object()};
    DWORD clipHash=hash(army.clip.memory,army.clip.bytes);FpEnv original=saveEnv();
    for(unsigned i=0;i<79;++i) {
        float* root=army.a[i]->root;
        if(i%4==1) {root[0]=root[5]=0;root[1]=-1;root[4]=1;}
        else if(i%4==2) {root[5]=root[10]=-1;}
        else if(i%4==3) {root[0]=root[10]=-1;}
        memcpy(army.b[i]->root,root,48);
    }
    for(unsigned precision:{0u,0x200u,0x300u}) for(unsigned rounding=0;rounding<4;++rounding) {
        FpEnv initial=original;initial.control=(WORD)((original.control&~0xF00)|precision|(rounding<<10)|63);initial.status=0;initial.tag=0xFFFF;
        for(unsigned i=0;i<79;++i) {army.a[i]->frame=army.b[i]->frame=(float)(i%17)+0.5f;}
        restoreEnv(initial);_mm_setcsr(0x1F80|(rounding<<13));poseRange(&a,0,79);FpEnv expected=saveEnv();DWORD expectedCsr=_mm_getcsr();
        restoreEnv(initial);_mm_setcsr(0x1F80|(rounding<<13));runPoses(posePool,b);FpEnv actual=saveEnv();
        assert(actual.control==expected.control && actual.tag==expected.tag && (actual.status&63)==(expected.status&63));
        assert(_mm_getcsr()==expectedCsr);army.compare();poses+=79*48;
    }
    restoreEnv(original);_mm_setcsr(0x1F80);assert(hash(army.clip.memory,army.clip.bytes)==clipHash);
    FpEnv unmasked=original;unmasked.control&=~1;restoreEnv(unmasked);assert(!posePool.run(b));restoreEnv(original);
    _mm_setcsr(0x1F00);assert(!posePool.run(b));_mm_setcsr(0x1F80);
    b.count=1;assert(!posePool.run(b));
    printf("PASS: %u color points including buffer tails, optional inputs and four SSE rounding modes; %u native bone outputs across 12 x87 modes; immutable clip; joined FP status\n",colors,poses);
}
static void measurements(AccelPreparePool& colors,NativePool& poses,unsigned cores) {
    for(unsigned capacity:{192u,4096u}) {
        const unsigned count=64;std::vector<float> rgb(count*capacity*3,0.4f),alpha(count*capacity,0.7f),a(count*capacity*4),b(a.size());
        std::vector<ColorJob> jobs;for(unsigned i=0;i<count;++i) jobs.push_back({b.data()+i*capacity*4,rgb.data()+i*capacity*3,alpha.data()+i*capacity,capacity,0});
        char name[100];sprintf(name,"%u cores 64x%u particle colors vs existing serial SIMD",cores,capacity);
        bench(name,1000,[&](){for(unsigned i=0;i<count;++i) particleColorPack(a.data()+i*capacity*4,jobs[i].rgb,jobs[i].alpha,capacity);},[&](){runColors(colors,jobs);});
        assert(!memcmp(a.data(),b.data(),a.size()*4));sink=*(DWORD*)b.data();
    }
    for(unsigned units:{32u,512u,1500u}) {
        Army army(units,64);PoseBatch a={army.a.data(),units,army.clip.object()},b={army.b.data(),units,army.clip.object()};
        for(unsigned i=0;i<units;++i) army.a[i]->frame=army.b[i]->frame=(float)(i%16);
        char name[100];sprintf(name,"%u cores %u independent native raw-animation trees/64 bones",cores,units);
        bench(name,units>512?50:100,[&](){poseRange(&a,0,units);},[&](){runPoses(poses,b);});army.compare();sink=*(DWORD*)(army.b[0]->pivots.data()+0x30);
    }
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2 || argc==3);
    if(strcmp(argv[1],"child")) return runMappedRetailChild(argv[1],argc==3);
    assert(hash((void*)0x563310,2850)==0xFFFABD60 && hash((void*)0x5A5050,345)==0x4B906A19);
    assert(hash((void*)0x47177E,733)==0x5E1E397E && hash((void*)0xB2BD10,512)==0x304D3B07);
    QueryPerformanceFrequency(&frequency);AccelCpuPlan full=accelAllowedCpuPlan();assert(full.count);
    DWORD_PTR originalMask,system;assert(GetProcessAffinityMask(GetCurrentProcess(),&originalMask,&system));
    for(unsigned cores=1;cores<=full.count && cores<=4;++cores) {
        DWORD_PTR allowed=0;for(unsigned i=0;i<cores;++i) allowed|=(DWORD_PTR)1<<full.cpu[i];
        assert(SetProcessAffinityMask(GetCurrentProcess(),allowed));DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),(DWORD_PTR)1<<full.cpu[0]);assert(previous);
        AccelCpuPlan plan=accelAllowedCpuPlan();assert(plan.count==cores);AccelPreparePool colors={};NativePool poses={};colors.start(plan);poses.start(plan);
        for(unsigned i=0;i<colors.workers;++i) assert(SetThreadAffinityMask(colors.threads[i],(DWORD_PTR)1<<plan.cpu[i+2]));
        printf("CORES %u physical, %u preparation workers; main and render roles reserved; workers pinned; render role idle in this fixture\n",cores,poses.workers);
        correctness(colors,poses);if(argc==3) measurements(colors,poses,cores);
        colors.shutdown();poses.shutdown();assert(SetProcessAffinityMask(GetCurrentProcess(),originalMask));assert(SetThreadAffinityMask(GetCurrentThread(),previous));
    }
    printf("PASS: whole-batch experiment only; no runtime hooks, game launch, D3D or game-frame FPS measurement\n");return 0;
}
