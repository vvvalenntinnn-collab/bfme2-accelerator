// Exercise installed retail hooks with native tree dispatch and controlled
// scene callbacks. The scene/GPU are deliberately not started by this fixture.
#include "aotr_accel.cpp"
#define main offloadFixtureMain
#include "rotwk_offload_test.cpp"
#undef main
static void realClip(Clip& c) {
    DWORD old;assert(VirtualProtect(c.memory,c.bytes,PAGE_READWRITE,&old));
    put(c.object(),0,(void*)0xBEBD90);put(c.object(),0x44,c.frames);put(c.object(),0x48,c.bones);
    assert(VirtualProtect(c.memory,c.bytes,PAGE_READONLY,&old));
}
static void* unitVt[128];
struct Unit {
    BYTE self[0x180];Skeleton skeleton;
    Unit(unsigned bones,unsigned index,Clip& clip):skeleton(bones,index) {
        // Exercise several matrix-to-quaternion branches and a non-exact
        // rotation under each tested x87 precision/rounding mode.
        float* r=skeleton.root;
        switch(index%5) {
        case 1: r[0]=r[5]=0;r[1]=-1;r[4]=1;break;
        case 2: r[5]=r[10]=-1;break;
        case 3: r[0]=r[10]=-1;break;
        case 4: r[0]=r[5]=0.8660254f;r[1]=-0.5f;r[4]=0.5f;break;
        }
        memset(self,0,sizeof(self));put(self,0,unitVt);memcpy(self+0x18,skeleton.root,48);
        put(self,0xF8,skeleton.tree);put(self,0x100,2u);put(self,0x104,clip.object());put(self,0x108,(float)(index%16));
    }
};
struct TestScene {
    BYTE self[0x120];struct Node {DWORD pad;void* next;void* previous;void* object;} ;
    std::vector<Node> nodes;
    TestScene(const std::vector<Unit*>& units):nodes(units.size()) {
        memset(self,0,sizeof(self));BYTE* head=self+0xF0;
        put(head,4,nodes.empty()?(void*)head:(void*)&nodes[0]);
        for(unsigned i=0;i<nodes.size();++i) {
            nodes[i]={0,i+1<nodes.size()?(void*)&nodes[i+1]:(void*)head,NULL,units[i]->self+8};
        }
    }
};
static std::vector<Unit*>* activeUnits;
static unsigned mutation,sceneCalls;
static bool referencePass;
static Clip* replacementClip;
static void editUnit(Unit& u,unsigned index) {
    if(index!=7) return;
    if(mutation==1) *(float*)(u.self+0x24)+=1.0f; // root translation
    if(mutation==2) *(float*)(u.self+0x108)+=0.5f; // animation frame
    if(mutation==3) *(float*)(u.skeleton.pivots.data()+88+0x24)+=0.125f; // base hierarchy
    if(mutation==4) *(float*)(u.skeleton.tree+0x18)=0.5f; // tree scale
    if(mutation==6) {
        BYTE* p=u.skeleton.pivots.data()+3*88;
        unsigned parent=*(BYTE**)(p+0x10)==u.skeleton.pivots.data()?1:0;
        put(p,0x10,u.skeleton.pivots.data()+parent*88);
    }
    if(mutation==7) put(u.self,0x104,replacementClip->object());
}
static void __fastcall sceneDispatch(void*,void*,void*) {
    ++sceneCalls;
    if(!referencePass && mutation==5 && g_rwSkelCount) g_rwSkelJobs[0].after[0x30]^=1;
    for(unsigned i=0;i<activeUnits->size();++i) {
        Unit& u=*(*activeUnits)[i];editUnit(u,i);
        if(referencePass) g_rwSkelOriginal(u.self,NULL);
        else ((tThis0)0x5A5050)(u.self,NULL);
    }
}
static void compareUnits(const std::vector<Unit*>& a,const std::vector<Unit*>& b) {
    assert(a.size()==b.size());
    for(unsigned i=0;i<a.size();++i) {
        assert(a[i]->self[0xF4]==b[i]->self[0xF4]);
        for(unsigned bone=0;bone<a[i]->skeleton.pivots.size()/88;++bone)
            assert(!memcmp(a[i]->skeleton.pivots.data()+bone*88+0x30,b[i]->skeleton.pivots.data()+bone*88+0x30,40));
    }
}
static void dirty(std::vector<Unit*>& units,unsigned frame) {
    for(unsigned i=0;i<units.size();++i) {units[i]->self[0xF4]=0;put(units[i]->self,0x108,(float)((i+frame)%16)+0.5f);}
}
static DWORD WINAPI foreignScene(void* scene) {((RwSkelSceneFn)0x47177E)(scene,NULL,NULL);return 0;}
static void layoutTests(Unit& u) {
    RwSkelJob j={};BYTE saved[sizeof(u.self)];memcpy(saved,u.self,sizeof(saved));u.self[0xF4]=0;
    assert(rwSkelLayout(u.self,j,0));
    for(unsigned offset:{0xFCu,0x7Cu,0x110u}) {put(u.self,offset,1u);assert(!rwSkelLayout(u.self,j,0));put(u.self,offset,0u);}
    put(u.self,0x100,3u);assert(!rwSkelLayout(u.self,j,0));put(u.self,0x100,2u);
    put(u.skeleton.tree,0x1C,1u);assert(!rwSkelLayout(u.self,j,0));put(u.skeleton.tree,0x1C,0u);
    BYTE* p=u.skeleton.pivots.data()+88;DWORD parent=*(DWORD*)(p+0x10),index=*(DWORD*)(p+0x4C);
    put(p,0x10,p);assert(!rwSkelLayout(u.self,j,0));put(p,0x10,parent);
    put(p,0x4C,0xFFFFFFFFu);assert(!rwSkelLayout(u.self,j,0));put(p,0x4C,index);
    assert(!rwSkelLayout(u.self,j,RW_SKEL_BONES-1));memcpy(u.self,saved,sizeof(saved));
    printf("PASS: private hierarchy rejects slaved/nested/auto-advanced/blended/captured/cyclic/out-of-capacity layouts\n");
}
static void runCases() {
    Clip clip(48),otherClip(48);realClip(clip);realClip(otherClip);replacementClip=&otherClip;std::vector<Unit*> a,b;
    for(unsigned i=0;i<79;++i) {a.push_back(new Unit(48,i,clip));b.push_back(new Unit(48,i,clip));}
    TestScene scene(b);g_rwSkelSceneOriginal=sceneDispatch;unsigned operations=0;
    RwSkelFp original=rwSkelSaveFp();
    for(unsigned iteration=0;iteration<72;++iteration) {
        mutation=iteration%7;if(mutation>=5) mutation+=1;g_mkFrames=iteration;dirty(a,iteration);dirty(b,iteration);
        RwSkelFp initial=original;initial.control=(WORD)((original.control&~0xF00)|((iteration%3==0)?0:((iteration%3==1)?0x200:0x300))|((iteration%4)<<10)|63);
        initial.status=0;initial.tag=0xFFFF;DWORD csr=0x1F80|((iteration%4)<<13)|((iteration&4)?0x8000:0)|((iteration&8)?0x40:0);
        rwSkelRestoreFp(initial);_mm_setcsr(csr);activeUnits=&a;referencePass=true;sceneDispatch(NULL,NULL,NULL);
        RwSkelFp expected=rwSkelSaveFp();DWORD expectedCsr=_mm_getcsr();
        rwSkelRestoreFp(initial);_mm_setcsr(csr);activeUnits=&b;referencePass=false;
        ((RwSkelSceneFn)0x47177E)(scene.self,NULL,NULL);RwSkelFp result=rwSkelSaveFp();
        assert(!g_rwSkelKill && expectedCsr==_mm_getcsr());
        assert(expected.control==result.control && expected.tag==result.tag && expected.status==result.status);
        compareUnits(a,b);operations+=79*48;
    }
    assert(!g_rwSkelProof && g_rwSkelTaken && g_rwSkelStale && g_rwSkelChecked>=512);
    // Busy pool and generation wrap retain native results and renew the cache.
    for(unsigned scenario=0;scenario<2;++scenario) {
        mutation=0;dirty(a,0);dirty(b,0);activeUnits=&a;referencePass=true;sceneDispatch(NULL,NULL,NULL);
        activeUnits=&b;referencePass=false;
        if(!scenario) g_rwPreparePool.busy=1;else g_rwSkelGeneration=0xFFFFFFFF;
        ((RwSkelSceneFn)0x47177E)(scene.self,NULL,NULL);g_rwPreparePool.busy=0;compareUnits(a,b);
    }
    // A foreign scene forwards without changing the main-thread batch/depth.
    mutation=0;activeUnits=&b;referencePass=true;unsigned oldDepth=g_rwSkelDepth;
    HANDLE thread=CreateThread(NULL,0,foreignScene,scene.self,0,NULL);assert(thread&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    assert(g_rwSkelDepth==oldDepth && !g_rwSkelCount);
    // Deliberately break a private proof output. Original results must still
    // be returned and all remaining updates fall back for the entire session.
    g_rwSkelProof=512;g_rwSkelKill=0;mutation=5;g_mkFrames++;
    dirty(a,0);dirty(b,0);activeUnits=&a;referencePass=true;sceneDispatch(NULL,NULL,NULL);
    activeUnits=&b;referencePass=false;((RwSkelSceneFn)0x47177E)(scene.self,NULL,NULL);
    assert(g_rwSkelKill);compareUnits(a,b);g_rwSkelKill=0;g_rwSkelProof=0;
    layoutTests(*b[0]);rwSkelRestoreFp(original);_mm_setcsr(0x1F80);
    for(auto u:a) delete u;for(auto u:b) delete u;
    printf("PASS: %u native bone outputs through installed hooks; proof/live transition, stale root/frame/base/scale rejection, foreign scene, mismatch kill and native forwarding\n",operations);
}
static void pipelineBenchmarks() {
    mutation=0;g_rwSkelProof=0;g_rwSkelKill=0;g_rwSkelSceneOriginal=sceneDispatch;
    for(unsigned n:{512u,1500u}) {
        Clip clip(64);realClip(clip);std::vector<Unit*> a,b;
        for(unsigned i=0;i<n;++i) {a.push_back(new Unit(64,i,clip));b.push_back(new Unit(64,i,clip));rwSkelRemember(b.back()->self);}
        TestScene scene(b);char name[120];sprintf(name,"LIVE pipeline %u units/64 bones including collect/snapshot/validate/commit",n);
        dirty(b,0);activeUnits=&b;referencePass=false;
        g_rwPreparePool.busy=1;LONG64 t0=now();rwSkelCollect(scene.self);LONG64 scan=now()-t0;g_rwPreparePool.busy=0;
        t0=now();rwSkelCollect(scene.self);LONG64 prepare=now()-t0;unsigned jobs=g_rwSkelCount;
        t0=now();sceneDispatch(NULL,NULL,NULL);LONG64 consume=now()-t0;g_rwSkelCount=0;
        printf("PROFILE %u units / %u jobs: scan %.3f us, scan+workers %.3f us, consume %.3f us\n",n,jobs,scan*1e6/frequency.QuadPart,prepare*1e6/frequency.QuadPart,consume*1e6/frequency.QuadPart);
        bench(name,80,[&](){dirty(a,0);activeUnits=&a;referencePass=true;sceneDispatch(NULL,NULL,NULL);},
            [&](){dirty(b,0);activeUnits=&b;referencePass=false;((RwSkelSceneFn)0x47177E)(scene.self,NULL,NULL);});
        compareUnits(a,b);for(auto u:a) delete u;for(auto u:b) delete u;
    }
}
int main(int argc,char** argv) {
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2 || argc==3);
    if(strcmp(argv[1],"child")) return runMappedRetailChild(argv[1],argc==3);
    InitializeCriticalSection(&g_logCs);QueryPerformanceFrequency(&g_pqpf);frequency=g_pqpf;g_mkTid=GetCurrentThreadId();unitVt[0xA8/4]=(void*)0x59CD70;
    AccelCpuPlan plan=accelAllowedCpuPlan();assert(plan.count>=3);assert(SetThreadAffinityMask(GetCurrentThread(),(DWORD_PTR)1<<plan.cpu[0]));
    g_rtIdealMain=plan.cpu[0];g_rtIdealWorker=plan.cpu[1];
    DWORD_PTR allowed,system;assert(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system));
    for(unsigned cores=1;cores<=2;++cores) {
        DWORD_PTR mask=plan.mask[0];if(cores==2) mask|=plan.mask[1];
        assert(SetProcessAffinityMask(GetCurrentProcess(),mask));assert(!rwInstallSkeletonWorkers());
    }
    assert(SetProcessAffinityMask(GetCurrentProcess(),allowed));
    SetEnvironmentVariableA("AOTR_SKELETONWORKERS","0");assert(!rwInstallSkeletonWorkers());SetEnvironmentVariableA("AOTR_SKELETONWORKERS",NULL);
    BYTE* getter=(BYTE*)0x58E9C0;BYTE saved=*getter;*getter^=1;assert(!rwInstallSkeletonWorkers());*getter=saved;
    assert(rwInstallSkeletonWorkers());assert(*(BYTE*)0x47177E==0xE9 && *(BYTE*)0x5A5050==0xE9);
    runCases();if(argc==3) pipelineBenchmarks();g_rwPreparePool.shutdown();
    printf("PASS: live retail skeleton capability, default activation, complete body/vtable guards and standalone native fixture\n");return 0;
}
