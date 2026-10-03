// Native mapped-code differential tests and synthetic workload measurements.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "aotr_accel.cpp"
#include "mapped_retail_test.h"
static DWORD seed=0x713569A9;
static DWORD randomWord() { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
template<class T> static void put(void* p,unsigned offset,T value) { memcpy((BYTE*)p+offset,&value,sizeof(value)); }
static volatile DWORD sink;
template<class Fn> static double timed(unsigned iterations,Fn fn) {
    LONG64 begin=qpcNow(); for(unsigned i=0;i<iterations;++i) fn(i);
    return (double)(qpcNow()-begin)*1e9/g_pqpf.QuadPart/iterations;
}
template<class A,class B> static void compareBench(const char* name,unsigned iterations,A native,B fast) {
    double a[7],b[7];
    for(unsigned i=0;i<7;++i) {
        if(i&1) { b[i]=timed(iterations,fast); a[i]=timed(iterations,native); }
        else { a[i]=timed(iterations,native); b[i]=timed(iterations,fast); }
    }
    std::sort(a,a+7); std::sort(b,b+7);
    printf("BENCH %s: native %.2f ns, candidate %.2f ns, %.2fx (seven-sample median)\n",name,a[3],b[3],a[3]/b[3]);
}
static unsigned failedEdit,editNumber;
static bool failingWrite(DWORD address,const BYTE* bytes,int size) {
    // Fail AFTER modifying memory: rollback must restore this edit too.
    bool ok=rwWriteEdit(address,bytes,size);
    return editNumber++==failedEdit?false:ok;
}
static void transactionTests() {
    RwHook pass={0x461CA3,259,0x2475C0B5,5,(void*)hkAlPass}; RwEdit edits[AL_NSITE+2];
    assert(rwPrepareAudioEdits(edits,&pass));
    for(failedEdit=0;failedEdit<_countof(edits);++failedEdit) {
        editNumber=0;
        assert(!rwCommitEdits(edits,_countof(edits),kRwAudioRegions,_countof(kRwAudioRegions),failingWrite));
        assert(rwRegionsMatch(kRwAudioRegions,_countof(kRwAudioRegions)));
    }
    BYTE* p=(BYTE*)0x456E88+13; BYTE saved=*p; *p^=1;
    assert(!rwCommitEdits(edits,_countof(edits),kRwAudioRegions,_countof(kRwAudioRegions))); *p=saved;
    printf("PASS: all 37 partial audio transactions roll back, including failure after write; region mismatch rejects installation\n");
}
static void topologyTests() {
    const DWORD_PTR smt[]={3,12,48,192},plain[]={1,2,4,8};
    AccelCpuPlan p=accelCpuPlan(smt,4,255,3);
    assert(p.count==4 && p.cpu[0]==3 && p.mask[0]==12 && p.cpu[1]==0);
    p=accelCpuPlan(smt,4,0x55,6); assert(p.count==4 && p.cpu[0]==6);
    p=accelCpuPlan(smt,4,3,0); assert(p.count==1);
    p=accelCpuPlan(plain,4,15,0); assert(p.count==4 && p.cpu[3]==3);
    p=accelCpuPlan(plain,4,0,0); assert(!p.count);
    p=accelReserveCpuRoles(accelCpuPlan(smt,4,255,4),1,3);
    assert(p.cpu[0]==1 && p.cpu[1]==3 && p.mask[0]==3 && p.mask[1]==12 && !(p.mask[2]&15));
    p=accelReserveCpuRoles(accelCpuPlan(smt,4,255,0),1,3);assert(p.cpu[0]==1 && p.cpu[1]==3);
    printf("PASS: physical-core planning, SMT siblings, partial affinity, no-SMT and single-core cases\n");
}
static void listTests() {
    SetEnvironmentVariableA("AOTR_RLSORT","0"); assert(!rwInstallList());
    assert(*(BYTE*)0x5740D0!=0xE9);
    SetEnvironmentVariableA("AOTR_RLSORT",NULL); assert(rwInstallList());
    const unsigned n=257; RlFakeObj objects[n]={}; RlEl stock[n],fast[n];
    for(unsigned i=0;i<n;++i) { objects[i].refs=1000000; objects[i].model=0x1000+(i%17)*64; }
    tRlPred pred=(tRlPred)kRlLessModel; tRlSort patched=(tRlSort)0x5740D0;
    for(unsigned rep=0;rep<3020;++rep) {
        for(unsigned i=0;i<n;++i) { stock[i]={}; stock[i].w[0]=(DWORD)&objects[randomWord()%n]; stock[i].w[1]=i; }
        memcpy(fast,stock,sizeof(stock)); o_rlSort(stock,stock+n,pred); patched(fast,fast+n,pred);
        assert(!memcmp(stock,fast,sizeof(stock)) && !g_rlOff);
    }
    assert(!g_rlCheckLeft && g_rlChecks>=3000);
    const unsigned big=2500; std::vector<RlEl> original(big),a(big),b(big);
    for(unsigned i=0;i<big;++i) { original[i]={}; original[i].w[0]=(DWORD)&objects[randomWord()%n]; original[i].w[1]=i; }
    compareBench("render list 2500 entries",200,[&](unsigned){a=original;o_rlSort(a.data(),a.data()+big,pred);},
        [&](unsigned){b=original;patched(b.data(),b.data()+big,pred);});
    assert(!memcmp(a.data(),b.data(),big*sizeof(RlEl)));
    printf("PASS: retail capability startup lists and 3020 hooked live sorts, proof-to-sampling transition\n");
}
// Call precisely the stock/patched 54-byte count loop with its real register
// and stack contract, independently of the audio device and Miles imports.
static void __declspec(naked) executeAudioLoop(DWORD mgr,DWORD event,DWORD* result,void* code) {
    __asm {
        push ebp
        mov ebp,esp
        sub esp,4
        push ebx
        push esi
        push edi
        mov ebx,[ebp+8]
        mov edi,[ebp+12]
        mov eax,[ebp+16]
        mov edx,[eax]
        mov [ebp-4],edx
        mov edx,[eax+4]
        mov [ebp+8],edx
        call dword ptr [ebp+20]
        mov edx,[ebp+16]
        mov eax,[ebp-4]
        mov [edx],eax
        mov eax,[ebp+8]
        mov [edx+4],eax
        pop edi
        pop esi
        pop ebx
        mov esp,ebp
        pop ebp
        ret
    }
}
struct AudioNode { AudioNode* next; AudioNode* prev; void* request; };
struct AudioFixture {
    BYTE manager[0xA0],event[4096][0x50]; DWORD request[4096][2]; AudioNode head,nodes[4096];
    void init(unsigned n,unsigned infos) {
        memset(this,0,sizeof(*this)); put(manager,0x98,&head); head.next=n?nodes:&head; head.prev=n?nodes+n-1:&head;
        for(unsigned i=0;i<n;++i) {
            nodes[i].next=i+1<n?nodes+i+1:&head; nodes[i].prev=i?nodes+i-1:&head;
            nodes[i].request=i%37?request[i]:NULL; request[i][1]=i%29?(DWORD)event[i]:0;
            put(event[i],8,(DWORD)(0x1000+(i%infos)*32)); event[i][0x4B]=(BYTE)(i%4==0);
        }
    }
};
static BYTE *nativeLoop,*patchedLoop;
static AudioFixture audio;
static void audioTests() {
    nativeLoop=(BYTE*)VirtualAlloc(NULL,64,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    patchedLoop=(BYTE*)VirtualAlloc(NULL,64,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(nativeLoop && patchedLoop); memcpy(nativeLoop,(void*)0x456E88,54); nativeLoop[54]=0xC3;
    SetEnvironmentVariableA("AOTR_AUDIOLIMIT","0"); assert(!rwInstallAudio());
    SetEnvironmentVariableA("AOTR_AUDIOLIMIT",NULL); assert(rwInstallAudio());
    memcpy(patchedLoop,(void*)0x456E88,54); patchedLoop[54]=0xC3;
    *(LONG*)(patchedLoop+3)=(LONG)((BYTE*)alCount-(patchedLoop+7));
    g_alProofLeft=0; unsigned cases=0;
    for(unsigned n:{0u,1u,31u,192u,512u,4096u}) for(unsigned infos:{1u,7u,64u}) {
        audio.init(n,infos); g_alInPass=1; g_alPassTid=GetCurrentThreadId(); g_alDirty=1; g_alSmall=0; g_alPassRebuilds=0;
        for(unsigned i=0;i<128;++i) {
            unsigned at=n?randomWord()%n:0; DWORD a[]={randomWord(),randomWord()},b[]={a[0],a[1]};
            executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[at],a,nativeLoop);
            executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[at],b,patchedLoop);
            assert(a[0]==b[0] && a[1]==b[1] && g_alLive); ++cases;
            if(n) audio.event[randomWord()%n][0x4B]^=1;
        }
        g_alInPass=0;
    }
    audio.init(192,1);for(unsigned i=0;i<192;++i) put(audio.event[i],8,0u);
    g_alInPass=1;g_alPassTid=GetCurrentThreadId();g_alDirty=1;g_alSmall=0;g_alPassRebuilds=0;
    DWORD expected[2]={},actual[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[1],expected,nativeLoop);
    executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[1],actual,patchedLoop);assert(!memcmp(expected,actual,sizeof(expected)));g_alInPass=0;
    printf("PASS: %u stock/patched native audio-loop cases, both stack counters, null entries, live flags and wrapping arithmetic\n",cases);
}
static void colorTests() {
    const DWORD bits[]={0,0x80000000,0x3F800000,0x40000000,0xBF800000,0x7F800000,0x7FC12345,0x7FA23456,1,0x80000001};
    const unsigned n=65536; std::vector<float> rgb(n*3),alpha(n),stock(n*4),fast(n*4);
    for(auto& value:rgb) { DWORD word=bits[randomWord()%_countof(bits)]; memcpy(&value,&word,4); }
    for(auto& value:alpha) { DWORD word=bits[randomWord()%_countof(bits)]; memcpy(&value,&word,4); }
    BYTE self[0x224]={},buffers[3][24]={}; put(self,0x100,n);put(self,0x218,buffers[0]); put(buffers[0],12,stock.data());
    put(buffers[1],12,rgb.data());put(buffers[2],12,alpha.data());
    AccelCpuPlan plan=accelAllowedCpuPlan(); assert(plan.count);
    AccelPreparePool pool={}; pool.start(plan); unsigned cases=0;
    for(unsigned length:{32769u,n}) for(unsigned control=0;control<16;++control) for(unsigned mode=1;mode<4;++mode) {
        put(self,0x100,length);std::fill(stock.begin(),stock.end(),-123.0f);std::fill(fast.begin(),fast.end(),-123.0f);
        put(self,0x21C,mode&1?buffers[1]:(BYTE*)NULL);put(self,0x220,mode&2?buffers[2]:(BYTE*)NULL);
        DWORD csr=0x1F80|((control&3)<<13)|((control&4)?0x8000:0)|((control&8)?0x40:0);
        _mm_setcsr(csr); ((RwColorFn)0x5A7DF0)(self,NULL); DWORD expectedCsr=_mm_getcsr();
        _mm_setcsr(csr); RwColorJob job={fast.data(),mode&1?rgb.data():NULL,mode&2?alpha.data():NULL};
        if(!pool.run(rwColorRange,&job,length,2048)) rwColorRange(&job,0,length);
        assert(!memcmp(stock.data(),fast.data(),n*16) && _mm_getcsr()==expectedCsr); ++cases;
    }
    RwColorJob job={fast.data(),rgb.data(),alpha.data()};
    pool.busy=1; assert(!pool.run(rwColorRange,&job,n,2048)); pool.busy=0;
    _mm_setcsr(0x1F00); assert(!pool.run(rwColorRange,&job,n,2048)); _mm_setcsr(0x1F80);
    pool.shutdown(); assert(!pool.workers); pool.start(plan); pool.shutdown();
    g_rwColorOriginal=(RwColorFn)0x5A7DF0;
    g_rwColorScratch=(float*)VirtualAlloc(NULL,n*16,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(g_rwColorScratch);
    g_rwPrepareEnabled=true;g_rwPrepareTried=false;
    for(unsigned control=0;control<16;++control) {
        DWORD csr=0x1F80|((control&3)<<13)|((control&4)?0x8000:0)|((control&8)?0x40:0);
        put(self,0x21C,buffers[1]);put(self,0x220,buffers[2]);put(buffers[0],12,stock.data());
        _mm_setcsr(csr);g_rwColorOriginal(self,NULL);DWORD expectedCsr=_mm_getcsr();
        put(buffers[0],12,fast.data());g_rwColorProof=1;g_rwColorTick=1;_mm_setcsr(csr);hkRwParticleColor(self,NULL);
        assert(!g_rwColorProof && !g_rwColorKill && !memcmp(stock.data(),fast.data(),n*16) && _mm_getcsr()==expectedCsr);
        _mm_setcsr(csr);hkRwParticleColor(self,NULL);
        assert(!g_rwColorKill && !memcmp(stock.data(),fast.data(),n*16) && _mm_getcsr()==expectedCsr);
    }
    g_rwPreparePool.shutdown();g_rwPrepareEnabled=false;g_rwPrepareTried=false;_mm_setcsr(0x1F80);
    printf("PASS: %u native/parallel color cases at 65536 capacity, NaNs/zeros, all MXCSR modes/status, shipping wrapper proof/live, busy/unmasked fallback and shutdown/restart\n",cases);
}
struct GatherFixture {
    BYTE self[0x38],buffers[6][24]; std::vector<BYTE> source[5],destination[5]; std::vector<DWORD> apt;
    void init(unsigned n,unsigned mask) {
        memset(self,0,sizeof(self));memset(buffers,0,sizeof(buffers));apt.resize(65536);
        for(unsigned i=0;i<apt.size();++i) apt[i]=randomWord()%65536;
        put(self,0xC,buffers[5]);put(self,0x1C,n);put(buffers[5],12,apt.data());put(buffers[5],16,65536u);
        for(unsigned f=0;f<5;++f) {
            source[f].resize(65536*kRwGatherWidth[f]+32);destination[f].resize(source[f].size());
            for(auto& b:source[f]) b=(BYTE)randomWord();std::fill(destination[f].begin(),destination[f].end(),0xA7);
            put(self,kRwGatherOffset[f],mask&(1<<f)?buffers[f]:(BYTE*)NULL);
            put(buffers[f],12,source[f].data());put(buffers[f],16,65536u);
            DWORD* v=(DWORD*)kRwGatherVector[f];v[1]=(DWORD)destination[f].data();v[2]=65536;
        }
    }
    void invoke(RwGatherFn function,void** output) { function(self,NULL,output,output+1,output+2,output+3,output+4); }
};
static GatherFixture gather;
static unsigned resizeCalls;
static void* gatherVtable[3];
static void __fastcall gatherResize(DWORD* v,void*,DWORD capacity,DWORD) { v[2]=capacity;++resizeCalls; }
static void gatherTests() {
    assert(!g_rwGatherOriginal);SetEnvironmentVariableA("AOTR_PARTICLEGATHER","1");installRotwkGather();
    assert(g_rwGatherOriginal && *(BYTE*)0x57EA80==0xE9);
    unsigned cases=0;
    for(unsigned mask=0;mask<32;++mask) for(unsigned n:{32u,193u,4096u,65536u}) {
        gather.init(n,mask);void* a[5],*b[5]; _mm_setcsr(0x1F80);gather.invoke(g_rwGatherOriginal,a);DWORD csr=_mm_getcsr();
        std::vector<BYTE> expected[5]; for(unsigned f=0;f<5;++f) { expected[f]=gather.destination[f];std::fill(gather.destination[f].begin(),gather.destination[f].end(),0xA7); }
        _mm_setcsr(0x1F80);gather.invoke(hkRwGather,b);
        assert(!memcmp(a,b,sizeof(a)) && _mm_getcsr()==csr && !g_rwGatherKill);
        for(unsigned f=0;f<5;++f) assert(expected[f]==gather.destination[f]);++cases;
    }
    gather.init(193,31);
    for(unsigned i=0;i<600;++i) {void* result[5];gather.invoke(hkRwGather,result);}
    assert(!g_rwGatherProof && !g_rwGatherKill);
    gatherVtable[2]=(void*)gatherResize;DWORD* v=(DWORD*)kRwGatherVector[0]; v[0]=(DWORD)gatherVtable;v[2]=1;
    void* result[5];gather.invoke(hkRwGather,result);assert(resizeCalls==1 && v[2]==386);
    // Preserve the no-APT branch's untouched optional out parameters.
    put(gather.self,0xC,(BYTE*)NULL);put(gather.self,8,(BYTE*)NULL);
    for(auto& p:result) p=(void*)0x1357;gather.invoke(hkRwGather,result);
    assert(result[0]==gather.source[0].data() && result[1]==(void*)0x1357);
    // An aliasing input/output case must retain the stock sequential copies.
    gather.init(193,31);put(gather.buffers[0],12,gather.destination[0].data());
    RwGatherJob job={};DWORD count;void** outputs[]={result,result+1,result+2,result+3,result+4};
    assert(!rwGatherLayout(gather.self,outputs,&job,&count));
    printf("PASS: %u native fused-gather cases, all optional fields, random ordered/duplicate indices, capacity tails, proof transition, native resize/default and alias fallback\n",cases);
}
static void coreBenchmarks() {
    AccelCpuPlan full=accelAllowedCpuPlan(); DWORD_PTR savedMask,system; GetProcessAffinityMask(GetCurrentProcess(),&savedMask,&system);
    DWORD_PTR allowed=0;
    for(unsigned cores=1;cores<=full.count && cores<=4;++cores) {
        allowed|=(DWORD_PTR)1<<full.cpu[cores-1]; assert(SetProcessAffinityMask(GetCurrentProcess(),allowed));
        DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),(DWORD_PTR)1<<full.cpu[0]); assert(previous);
        AccelCpuPlan plan=accelAllowedCpuPlan(); assert(plan.count==cores);
        g_rwPreparePool.shutdown(); g_rwPreparePool.start(plan); g_rwPrepareTried=true; g_rwPrepareEnabled=true;
        printf("CORES %u physical (one logical CPU per core), preparation workers %u; no D3D/GPU workload\n",cores,g_rwPreparePool.workers);
        for(unsigned n:{192u,4096u,65536u}) {
            std::vector<float> rgb(n*3,0.4f),alpha(n,0.7f),out(n*4);
            BYTE self[0x224]={},buffer[3][24]={}; put(self,0x100,n);put(self,0x218,buffer[0]);put(self,0x21C,buffer[1]);put(self,0x220,buffer[2]);
            put(buffer[0],12,out.data());put(buffer[1],12,rgb.data());put(buffer[2],12,alpha.data());
            char name[80];sprintf(name,"%u cores color capacity %u",cores,n);
            compareBench(name,n<1000?10000:1000,[&](unsigned){((RwColorFn)0x5A7DF0)(self,NULL);},
                [&](unsigned){rwPackColors(out.data(),rgb.data(),alpha.data(),n);});
            sink=*(DWORD*)out.data();
        }
        for(unsigned n:{32768u,65536u}) {
            std::vector<float> rgb(n*3,0.4f),alpha(n,0.7f),out(n*4);char name[96];
            sprintf(name,"%u cores joined pool versus serial SIMD color %u",cores,n);
            compareBench(name,1000,[&](unsigned){particleColorPack(out.data(),rgb.data(),alpha.data(),n);},
                [&](unsigned){rwPackColors(out.data(),rgb.data(),alpha.data(),n);});sink=*(DWORD*)out.data();
        }
        for(unsigned n:{192u,512u,4096u}) {
            audio.init(n,64); g_alInPass=1;g_alPassTid=GetCurrentThreadId();g_alDirty=1;g_alSmall=0;g_alPassRebuilds=0;
            char name[80];sprintf(name,"%u cores audio count %u requests/64 sounds",cores,n);
            compareBench(name,2000,[&](unsigned i){DWORD result[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[i%n],result,nativeLoop);sink=result[0];},
                [&](unsigned i){DWORD result[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[i%n],result,patchedLoop);sink=result[0];});
            g_alInPass=0;
        }
        audio.init(512,1);g_alInPass=1;g_alPassTid=GetCurrentThreadId();g_alDirty=1;g_alSmall=0;g_alPassRebuilds=0;
        char denseName[80];sprintf(denseName,"%u cores audio dense bucket 512 requests/1 sound",cores);
        compareBench(denseName,2000,[&](unsigned i){DWORD result[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[i%512],result,nativeLoop);sink=result[0];},
            [&](unsigned i){DWORD result[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[i%512],result,patchedLoop);sink=result[0];});g_alInPass=0;
        for(unsigned n:{192u,4096u,65536u}) {
            gather.init(n,31);void* result[5];char name[80];sprintf(name,"%u cores selected-particle gather %u",cores,n);
            compareBench(name,500,[&](unsigned){gather.invoke(g_rwGatherOriginal,result);},[&](unsigned){gather.invoke(hkRwGather,result);});
            assert(!g_rwGatherKill);sink=*(DWORD*)result[0];
        }
        // A repeatable dense CPU workload, not a simulated game frame: one
        // render list, all audio checks and 64 ordinary-size particle systems.
        for(unsigned load=0;load<2;++load) {
            unsigned meshes=load?10000:2500,requests=load?4096:512,capacity=4096;
            std::vector<RlFakeObj> objects(256);std::vector<RlEl> original(meshes),sorted(meshes);
            for(unsigned i=0;i<objects.size();++i) {objects[i]={};objects[i].refs=1000000;objects[i].model=0x1000+(i%17)*64;}
            for(auto& e:original) {e={};e.w[0]=(DWORD)&objects[randomWord()%objects.size()];e.w[1]=randomWord();}
            std::vector<float> rgb(64*capacity*3,0.4f),alpha(64*capacity,0.7f),out(64*capacity*4);
            BYTE object[0x224]={},buffers[3][24]={};put(object,0x100,capacity);put(object,0x218,buffers[0]);put(object,0x21C,buffers[1]);put(object,0x220,buffers[2]);
            put(buffers[0],12,out.data());put(buffers[1],12,rgb.data());put(buffers[2],12,alpha.data());audio.init(requests,64);
            auto pass=[&](bool accelerated) {
                sorted=original;tRlPred predicate=(tRlPred)kRlLessModel;
                if(accelerated) ((tRlSort)0x5740D0)(sorted.data(),sorted.data()+meshes,predicate);else o_rlSort(sorted.data(),sorted.data()+meshes,predicate);
                g_alInPass=1;g_alPassTid=GetCurrentThreadId();g_alDirty=1;g_alSmall=0;g_alPassRebuilds=0;
                for(unsigned i=0;i<requests;++i) {DWORD result[2]={};executeAudioLoop((DWORD)audio.manager,(DWORD)audio.event[i],result,accelerated?patchedLoop:nativeLoop);sink=result[0];}
                g_alInPass=0;
                for(unsigned i=0;i<64;++i) {
                    float* dst=out.data()+i*capacity*4;const float* colors=rgb.data()+i*capacity*3;const float* opacity=alpha.data()+i*capacity;
                    put(buffers[0],12,dst);put(buffers[1],12,colors);put(buffers[2],12,opacity);
                    if(accelerated) rwPackColors(dst,colors,opacity,capacity);else ((RwColorFn)0x5A7DF0)(object,NULL);
                }
                sink=*(DWORD*)out.data();
            };
            char name[96];sprintf(name,"%u cores dense CPU workload %u meshes/%u requests/64x4096 colors",cores,meshes,requests);
            compareBench(name,load?10:50,[&](unsigned){pass(false);},[&](unsigned){pass(true);});
        }
        g_rwPreparePool.shutdown();
        assert(SetProcessAffinityMask(GetCurrentProcess(),savedMask)); assert(SetThreadAffinityMask(GetCurrentThread(),previous));
    }
}
#ifndef AOTR_PROD
static DWORD phaseArgument; static unsigned phaseCalls;
static void __fastcall fakeLogic(void*,void*,DWORD n) { assert(g_phase==PH_LOGIC);phaseArgument=n;++phaseCalls; }
static void __fastcall fakeClient(void*,void*) { assert(g_phase==(GetCurrentThreadId()==g_mkTid?PH_CLIENT:PH_OTHER));++phaseCalls; }
static void __fastcall fakeOther(void*,void*) { ++phaseCalls; }
static DWORD WINAPI foreignClient(void*) { hkRwClient(NULL,NULL);return 0; }
static void phaseTests() {
    g_diag=1; DWORD hash=rwBodyHash((const BYTE*)0x6329B0,356)==0x1698D50B?0x88C193EE:0x4404F3C6;
    assert(rwInstallPhaseTimers(hash,false)); assert(*(BYTE*)0x6329B0==0xE9 && *(BYTE*)0x6329B5==0x90);
    g_rwLogicOriginal=fakeLogic;g_rwClientOriginal=fakeClient;g_rwManagerOriginal=fakeOther;g_rwPathOriginal=fakeOther;
    LONG previous=g_phase=PH_OTHER;hkRwLogic(NULL,NULL,7);hkRwClient(NULL,NULL);hkRwManager(NULL,NULL);hkRwPath(NULL,NULL);
    assert(phaseCalls==4 && phaseArgument==7 && g_phase==previous);
    for(unsigned i=0;i<RW_PHASES;++i) assert(g_rwPhaseCalls[i]==1 && g_rwPhaseTicks[i]>=0);
    HANDLE thread=CreateThread(NULL,0,foreignClient,NULL,0,NULL);assert(thread && WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    assert(g_phase==PH_OTHER && g_rwPhaseCalls[RW_CLIENT]==2);
    printf("PASS: EXE/DAT-specific coarse profiling guards, complete eight-byte logic detour and wrapper forwarding\n");
}
static void diagnosticTests() {
    struct {RtRec r;DWORD a[7];} a={},b={},barrier={};a.r.kind=b.r.kind=1;a.r.op=b.r.op=82;a.r.nargs=b.r.nargs=7;a.r.fn=b.r.fn=(void*)0x1357;
    DWORD first[]={1,4,0,0,12,10,4},second[]={1,4,0,12,9,22,3};memcpy(a.a,first,sizeof(first));memcpy(b.a,second,sizeof(second));
    RtRec* previous=NULL;rtObserveDrawRange(previous,&a.r);rtObserveDrawRange(previous,&b.r);
    assert(g_rtDrawRecords==2 && g_rtAdjacentDrawRanges==1);
    rtObserveDrawRange(previous,&barrier.r);rtObserveDrawRange(previous,&b.r);assert(g_rtAdjacentDrawRanges==1);
    b.a[2]=1;assert(!accelAdjacentDrawRanges(82,a.a,b.a));b.a[2]=0;
    b.a[1]=5;assert(!accelAdjacentDrawRanges(82,a.a,b.a));b.a[1]=4;
    a.a[5]=0xFFFFFFFE;assert(!accelAdjacentDrawRanges(82,a.a,b.a));
    DWORD dp1[]={1,4,7,2},dp2[]={1,4,13,3};assert(accelAdjacentDrawRanges(81,dp1,dp2));
    assert(rtIntervalBin(999,120000)==33 && rtIntervalBin(1920,120000)==64 && rtIntervalBin(5760,120000)==80);
    LONG64 frequency=g_pqpf.QuadPart;g_pqpf.QuadPart=120000;
    rtRecordPortableInterval(1000);rtRecordPortableInterval(1001);rtRecordPortableInterval(0);
    assert(g_rtPortableIntervals==2 && g_rtPortableOverBudget==1 && g_rtPortableMaximum==1001);g_pqpf.QuadPart=frequency;
    printf("PASS: draw-range candidate barriers, unsupported primitive/base/overflow rejection and exact 120Hz reporting boundary\n");
}
#endif
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2 || argc==3);
    if(strcmp(argv[1],"child")) return runMappedRetailChild(argv[1],argc==3);
    InitializeCriticalSection(&g_logCs); QueryPerformanceFrequency(&g_pqpf);g_mkTid=GetCurrentThreadId();
    topologyTests();transactionTests();listTests();audioTests();colorTests();gatherTests();
    if(argc==3) coreBenchmarks();
#ifndef AOTR_PROD
    phaseTests();diagnosticTests();
#endif
    g_rwPreparePool.shutdown();DeleteCriticalSection(&g_logCs);printf("ALL SYNTHETIC CHECKS PASSED\n");return 0;
}
